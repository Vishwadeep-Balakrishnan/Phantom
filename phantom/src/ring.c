#include "ring.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * MurmurHash3 finalizer — good avalanche, fast, no deps.
 */
static uint32_t murmur3_32(const uint8_t *key, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    uint32_t k;
    size_t i;

    for (i = len >> 2; i; i--) {
        memcpy(&k, key, sizeof(uint32_t));
        key += sizeof(uint32_t);
        k *= 0xcc9e2d51u;
        k  = (k << 15) | (k >> 17);
        k *= 0x1b873593u;
        h ^= k;
        h  = (h << 13) | (h >> 19);
        h  = h * 5 + 0xe6546b64u;
    }

    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)key[2] << 16; /* fall through */
    case 2: k ^= (uint32_t)key[1] << 8;  /* fall through */
    case 1: k ^= key[0];
            k *= 0xcc9e2d51u;
            k  = (k << 15) | (k >> 17);
            k *= 0x1b873593u;
            h ^= k;
    }

    h ^= (uint32_t)len;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;

    return h;
}

static int entry_cmp(const void *a, const void *b)
{
    const ring_entry_t *ea = a;
    const ring_entry_t *eb = b;
    if (ea->token < eb->token) return -1;
    if (ea->token > eb->token) return  1;
    return 0;
}

void hash_ring_init(hash_ring_t *ring)
{
    ring->count = 0;
    pthread_rwlock_init(&ring->lock, NULL);
}

void hash_ring_destroy(hash_ring_t *ring)
{
    pthread_rwlock_destroy(&ring->lock);
}

int hash_ring_add_node(hash_ring_t *ring, int node_id)
{
    char buf[64];
    int  added = 0;

    pthread_rwlock_wrlock(&ring->lock);

    if (ring->count + RING_VNODES_PER_NODE > RING_MAX_TOKENS) {
        pthread_rwlock_unlock(&ring->lock);
        return -1;
    }

    for (int v = 0; v < RING_VNODES_PER_NODE; v++) {
        int len = snprintf(buf, sizeof(buf), "node-%d-vnode-%d", node_id, v);
        uint32_t token = murmur3_32((uint8_t *)buf, (size_t)len, 0xdeadbeef);
        ring->entries[ring->count].token   = token;
        ring->entries[ring->count].node_id = node_id;
        ring->count++;
        added++;
    }

    qsort(ring->entries, ring->count, sizeof(ring_entry_t), entry_cmp);

    pthread_rwlock_unlock(&ring->lock);
    return added;
}

int hash_ring_remove_node(hash_ring_t *ring, int node_id)
{
    int removed = 0;

    pthread_rwlock_wrlock(&ring->lock);

    for (int i = 0; i < ring->count; ) {
        if (ring->entries[i].node_id == node_id) {
            ring->entries[i] = ring->entries[ring->count - 1];
            ring->count--;
            removed++;
        } else {
            i++;
        }
    }

    if (removed > 0)
        qsort(ring->entries, ring->count, sizeof(ring_entry_t), entry_cmp);

    pthread_rwlock_unlock(&ring->lock);
    return removed;
}

int hash_ring_get_node(hash_ring_t *ring, const void *key, size_t key_len)
{
    if (ring->count == 0)
        return -1;

    uint32_t h = murmur3_32(key, key_len, 0xdeadbeef);

    pthread_rwlock_rdlock(&ring->lock);

    int lo = 0, hi = ring->count - 1, mid, result = 0;
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
        if (ring->entries[mid].token < h)
            lo = mid + 1;
        else {
            result = mid;
            hi = mid - 1;
        }
    }

    /* wrap around */
    if (ring->entries[result].token < h)
        result = 0;

    int node_id = ring->entries[result].node_id;
    pthread_rwlock_unlock(&ring->lock);

    return node_id;
}

int hash_ring_get_replicas(hash_ring_t *ring, const void *key, size_t key_len,
                           int *nodes, int n)
{
    if (ring->count == 0)
        return 0;

    uint32_t h = murmur3_32(key, key_len, 0xdeadbeef);

    pthread_rwlock_rdlock(&ring->lock);

    int lo = 0, hi = ring->count - 1, mid, start = 0;
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
        if (ring->entries[mid].token < h)
            lo = mid + 1;
        else {
            start = mid;
            hi = mid - 1;
        }
    }
    if (ring->entries[start].token < h)
        start = 0;

    int found = 0;
    for (int i = 0; i < ring->count && found < n; i++) {
        int idx = (start + i) % ring->count;
        int nid = ring->entries[idx].node_id;
        int dup = 0;
        for (int j = 0; j < found; j++)
            if (nodes[j] == nid) { dup = 1; break; }
        if (!dup)
            nodes[found++] = nid;
    }

    pthread_rwlock_unlock(&ring->lock);
    return found;
}
