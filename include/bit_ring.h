// bit_ring.h
#include <stdatomic.h>
#include <stdlib.h>

typedef struct {
    uint8_t *bits;
    size_t capacity;
    _Atomic size_t head, tail;
} bit_ring_t;

static inline void bit_ring_init(bit_ring_t *r, size_t capacity) {
    r->bits = calloc(capacity, 1);
    r->capacity = capacity;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}

static inline size_t bit_ring_write(bit_ring_t *r, const uint8_t *bits, size_t n) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    size_t free_space = r->capacity - (head - tail);
    size_t to_write = n < free_space ? n : free_space;
    for (size_t i = 0; i < to_write; i++)
        r->bits[(head + i) % r->capacity] = bits[i];
    atomic_store_explicit(&r->head, head + to_write, memory_order_release);
    return to_write;
}

// returns 1 if a bit was available, 0 on underrun
static inline int bit_ring_read1(bit_ring_t *r, uint8_t *out) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head == tail) return 0;
    *out = r->bits[tail % r->capacity];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return 1;
}