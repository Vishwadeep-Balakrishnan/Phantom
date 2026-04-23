#ifndef PHANTOM_STORE_H
#define PHANTOM_STORE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define STORE_INITIAL_CAP  4096
#define STORE_MAX_KEY_LEN  256
#define STORE_MAX_VAL_LEN  65536
#define STORE_LOAD_FACTOR  0.70

typedef struct {
    char    *key;
    size_t   key_len;
    char    *val;
    size_t   val_len;
    uint64_t version;   /* monotonic, for last-write-wins */
    int      tombstone; /* 1 = deleted */
} store_entry_t;

typedef struct {
    store_entry_t *slots;
    size_t         cap;
    size_t         used;
    size_t         tombstones;
    uint64_t       version_clock;
    pthread_rwlock_t lock;
} store_t;

int  store_init(store_t *s);
void store_destroy(store_t *s);
int  store_put(store_t *s, const char *key, size_t klen,
               const char *val, size_t vlen, uint64_t version);
int  store_get(store_t *s, const char *key, size_t klen,
               char **val_out, size_t *vlen_out);
int  store_del(store_t *s, const char *key, size_t klen);
size_t store_count(store_t *s);

#endif
