#ifndef PHANTOM_QUEUE_H
#define PHANTOM_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/*
 * Lock-free MPMC bounded queue based on Dmitry Vyukov's design.
 * Each slot has its own sequence number so producers and consumers
 * can coordinate without a global lock.
 *
 * capacity must be a power of two.
 */

typedef struct {
    void       *data;
    atomic_size_t sequence;
} mpmc_slot_t;

typedef struct {
    char         _pad0[64];
    atomic_size_t enqueue_pos;
    char         _pad1[64 - sizeof(atomic_size_t)];
    atomic_size_t dequeue_pos;
    char         _pad2[64 - sizeof(atomic_size_t)];
    size_t        capacity;
    size_t        mask;
    mpmc_slot_t  *slots;
} mpmc_queue_t;

int    mpmc_queue_init(mpmc_queue_t *q, size_t capacity);
void   mpmc_queue_destroy(mpmc_queue_t *q);
int    mpmc_queue_push(mpmc_queue_t *q, void *data);
int    mpmc_queue_pop(mpmc_queue_t *q, void **data);
size_t mpmc_queue_size(mpmc_queue_t *q);

#endif
