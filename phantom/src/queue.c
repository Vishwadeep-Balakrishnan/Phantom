#include "queue.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int mpmc_queue_init(mpmc_queue_t *q, size_t capacity)
{
    if (capacity == 0 || (capacity & (capacity - 1)) != 0)
        return -EINVAL;

    q->slots = malloc(sizeof(mpmc_slot_t) * capacity);
    if (!q->slots)
        return -ENOMEM;

    q->capacity = capacity;
    q->mask = capacity - 1;

    for (size_t i = 0; i < capacity; i++)
        atomic_store_explicit(&q->slots[i].sequence, i, memory_order_relaxed);

    atomic_store_explicit(&q->enqueue_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&q->dequeue_pos, 0, memory_order_relaxed);

    return 0;
}

void mpmc_queue_destroy(mpmc_queue_t *q)
{
    free(q->slots);
    q->slots = NULL;
}

int mpmc_queue_push(mpmc_queue_t *q, void *data)
{
    mpmc_slot_t *slot;
    size_t pos, seq;
    intptr_t diff;

    pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);

    for (;;) {
        slot = &q->slots[pos & q->mask];
        seq  = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        diff = (intptr_t)seq - (intptr_t)pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->enqueue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            /* queue is full */
            return -EAGAIN;
        } else {
            pos = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
        }
    }

    slot->data = data;
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);

    return 0;
}

int mpmc_queue_pop(mpmc_queue_t *q, void **data)
{
    mpmc_slot_t *slot;
    size_t pos, seq;
    intptr_t diff;

    pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);

    for (;;) {
        slot = &q->slots[pos & q->mask];
        seq  = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        diff = (intptr_t)seq - (intptr_t)(pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &q->dequeue_pos, &pos, pos + 1,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        } else if (diff < 0) {
            /* queue is empty */
            return -EAGAIN;
        } else {
            pos = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
        }
    }

    *data = slot->data;
    atomic_store_explicit(&slot->sequence, pos + q->mask + 1, memory_order_release);

    return 0;
}

size_t mpmc_queue_size(mpmc_queue_t *q)
{
    size_t enq = atomic_load_explicit(&q->enqueue_pos, memory_order_relaxed);
    size_t deq = atomic_load_explicit(&q->dequeue_pos, memory_order_relaxed);
    return (enq >= deq) ? (enq - deq) : 0;
}
