#include "store.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Open-addressing hash table with Robin Hood probing.
 * Robin Hood keeps probe distances short by stealing slots from
 * entries that are "closer to home" than the incoming one.
 * This gives better worst-case lookup than vanilla linear probing.
 */

static uint64_t hash_key(const char *key, size_t len)
{
    /* FNV-1a 64-bit */
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)key[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t probe_dist(store_t *s, size_t idx)
{
    if (!s->slots[idx].key)
        return 0;
    size_t ideal = (size_t)(hash_key(s->slots[idx].key, s->slots[idx].key_len) % s->cap);
    return (idx + s->cap - ideal) % s->cap;
}

static int store_resize(store_t *s)
{
    size_t new_cap = s->cap * 2;
    store_entry_t *new_slots = calloc(new_cap, sizeof(store_entry_t));
    if (!new_slots)
        return -ENOMEM;

    for (size_t i = 0; i < s->cap; i++) {
        store_entry_t *e = &s->slots[i];
        if (!e->key || e->tombstone)
            continue;

        size_t idx = (size_t)(hash_key(e->key, e->key_len) % new_cap);
        store_entry_t cur = *e;

        while (1) {
            if (!new_slots[idx].key) {
                new_slots[idx] = cur;
                break;
            }
            size_t cur_dist = (idx + new_cap - (size_t)(hash_key(cur.key, cur.key_len) % new_cap)) % new_cap;
            size_t occ_dist = (idx + new_cap - (size_t)(hash_key(new_slots[idx].key, new_slots[idx].key_len) % new_cap)) % new_cap;
            if (cur_dist > occ_dist) {
                store_entry_t tmp = new_slots[idx];
                new_slots[idx] = cur;
                cur = tmp;
            }
            idx = (idx + 1) % new_cap;
        }
    }

    free(s->slots);
    s->slots = new_slots;
    s->cap   = new_cap;
    s->tombstones = 0;
    return 0;
}

int store_init(store_t *s)
{
    s->slots = calloc(STORE_INITIAL_CAP, sizeof(store_entry_t));
    if (!s->slots)
        return -ENOMEM;
    s->cap = STORE_INITIAL_CAP;
    s->used = 0;
    s->tombstones = 0;
    s->version_clock = 1;
    pthread_rwlock_init(&s->lock, NULL);
    return 0;
}

void store_destroy(store_t *s)
{
    for (size_t i = 0; i < s->cap; i++) {
        free(s->slots[i].key);
        free(s->slots[i].val);
    }
    free(s->slots);
    pthread_rwlock_destroy(&s->lock);
}

int store_put(store_t *s, const char *key, size_t klen,
              const char *val, size_t vlen, uint64_t version)
{
    if (klen == 0 || klen > STORE_MAX_KEY_LEN) return -EINVAL;
    if (vlen > STORE_MAX_VAL_LEN)              return -EINVAL;

    pthread_rwlock_wrlock(&s->lock);

    if ((double)(s->used + s->tombstones) / (double)s->cap > STORE_LOAD_FACTOR) {
        if (store_resize(s) < 0) {
            pthread_rwlock_unlock(&s->lock);
            return -ENOMEM;
        }
    }

    uint64_t ver = version ? version : s->version_clock++;
    size_t idx = (size_t)(hash_key(key, klen) % s->cap);

    store_entry_t incoming = {
        .key       = strndup(key, klen),
        .key_len   = klen,
        .val       = vlen ? strndup(val, vlen) : NULL,
        .val_len   = vlen,
        .version   = ver,
        .tombstone = 0,
    };

    if (!incoming.key) {
        pthread_rwlock_unlock(&s->lock);
        return -ENOMEM;
    }

    size_t dist = 0;
    while (1) {
        store_entry_t *slot = &s->slots[idx];

        if (!slot->key || slot->tombstone) {
            if (!slot->key) s->used++;
            if (slot->tombstone) s->tombstones--;
            free(slot->key);
            free(slot->val);
            *slot = incoming;
            break;
        }

        if (slot->key_len == klen && memcmp(slot->key, key, klen) == 0) {
            if (ver >= slot->version) {
                free(slot->val);
                slot->val       = incoming.val;
                slot->val_len   = vlen;
                slot->version   = ver;
                slot->tombstone = 0;
                free(incoming.key);
            } else {
                free(incoming.key);
                free(incoming.val);
            }
            break;
        }

        size_t occ_dist = probe_dist(s, idx);
        if (dist > occ_dist) {
            store_entry_t tmp = *slot;
            *slot = incoming;
            incoming = tmp;
            dist = occ_dist;
        }

        idx = (idx + 1) % s->cap;
        dist++;
    }

    pthread_rwlock_unlock(&s->lock);
    return 0;
}

int store_get(store_t *s, const char *key, size_t klen,
              char **val_out, size_t *vlen_out)
{
    if (!key || klen == 0)
        return -EINVAL;

    pthread_rwlock_rdlock(&s->lock);

    size_t idx  = (size_t)(hash_key(key, klen) % s->cap);
    size_t dist = 0;
    int    rc   = -ENOENT;

    while (1) {
        store_entry_t *slot = &s->slots[idx];

        if (!slot->key || probe_dist(s, idx) < dist)
            break;

        if (!slot->tombstone && slot->key_len == klen &&
            memcmp(slot->key, key, klen) == 0)
        {
            if (val_out) {
                *val_out = malloc(slot->val_len + 1);
                if (*val_out) {
                    memcpy(*val_out, slot->val, slot->val_len);
                    (*val_out)[slot->val_len] = '\0';
                    *vlen_out = slot->val_len;
                    rc = 0;
                } else {
                    rc = -ENOMEM;
                }
            } else {
                rc = 0;
            }
            break;
        }

        idx = (idx + 1) % s->cap;
        dist++;
    }

    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int store_del(store_t *s, const char *key, size_t klen)
{
    if (!key || klen == 0)
        return -EINVAL;

    pthread_rwlock_wrlock(&s->lock);

    size_t idx  = (size_t)(hash_key(key, klen) % s->cap);
    size_t dist = 0;
    int    rc   = -ENOENT;

    while (1) {
        store_entry_t *slot = &s->slots[idx];

        if (!slot->key || probe_dist(s, idx) < dist)
            break;

        if (slot->key_len == klen && memcmp(slot->key, key, klen) == 0) {
            if (!slot->tombstone) {
                slot->tombstone = 1;
                s->tombstones++;
            }
            rc = 0;
            break;
        }

        idx = (idx + 1) % s->cap;
        dist++;
    }

    pthread_rwlock_unlock(&s->lock);
    return rc;
}

size_t store_count(store_t *s)
{
    pthread_rwlock_rdlock(&s->lock);
    size_t n = s->used - s->tombstones;
    pthread_rwlock_unlock(&s->lock);
    return n;
}
