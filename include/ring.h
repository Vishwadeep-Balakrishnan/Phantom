#ifndef PHANTOM_RING_H
#define PHANTOM_RING_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define RING_VNODES_PER_NODE  150
#define RING_MAX_NODES        64
#define RING_MAX_TOKENS       (RING_MAX_NODES * RING_VNODES_PER_NODE)

typedef struct {
    uint32_t token;      /* position on the ring [0, UINT32_MAX] */
    int      node_id;
} ring_entry_t;

typedef struct {
    ring_entry_t  entries[RING_MAX_TOKENS];
    int           count;
    pthread_rwlock_t lock;
} hash_ring_t;

void hash_ring_init(hash_ring_t *ring);
void hash_ring_destroy(hash_ring_t *ring);
int  hash_ring_add_node(hash_ring_t *ring, int node_id);
int  hash_ring_remove_node(hash_ring_t *ring, int node_id);
int  hash_ring_get_node(hash_ring_t *ring, const void *key, size_t key_len);
int  hash_ring_get_replicas(hash_ring_t *ring, const void *key, size_t key_len,
                            int *nodes, int n);

#endif
