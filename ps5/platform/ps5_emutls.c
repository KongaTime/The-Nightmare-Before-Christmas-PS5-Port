/*
 * clang lowers PS5 __thread access to compiler-rt's emulated TLS. Cache
 * those addresses in rows owned by the current pthread. %fs:16 supplies
 * pthread_self() on this target. Only the owning pthread reads or writes
 * a row's cells.
 */
#include <stdint.h>
#include <stddef.h>

extern void *__real___emutls_get_address(void *control);

#define EMUTLS_ROWS 1024u
#define EMUTLS_CELLS 128u

typedef struct {
    const void *control;
    void *ptr;
} EmutlsCell;

/* Keep the owner probes compact and each 2 KiB cell row a power-of-two stride.
 * This also avoids multiplying by the combined owner-and-cells struct size. */
static uintptr_t s_owners[EMUTLS_ROWS];
static EmutlsCell s_cells[EMUTLS_ROWS][EMUTLS_CELLS];

static unsigned control_slot(const void *control)
{
    return ((uint32_t)(uintptr_t)control * 2654435761u) >> 25;
}

static __attribute__((noinline)) void *
cache_miss(void *control, uintptr_t self, unsigned row, unsigned slot)
{
    for (unsigned probe = 0; probe < EMUTLS_ROWS; ++probe) {
        uintptr_t owner = __atomic_load_n(&s_owners[row], __ATOMIC_RELAXED);
        if (!owner) {
            __atomic_compare_exchange_n(&s_owners[row], &owner, self, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED);
            if (!owner)
                owner = self;
        }
        if (owner == self) {
            EmutlsCell *a = &s_cells[row][slot];
            EmutlsCell *b = &s_cells[row][slot ^ 1u];
            if (a->control == control)
                return a->ptr;
            if (b->control == control)
                return b->ptr;
            void *ptr = __real___emutls_get_address(control);
            EmutlsCell *cell = a->control && !b->control ? b : a;
            cell->ptr = ptr;
            __atomic_store_n(&cell->control, control, __ATOMIC_RELAXED);
            return ptr;
        }
        row = (row + 1u) & (EMUTLS_ROWS - 1u);
    }
    return __real___emutls_get_address(control);
}

void *__wrap___emutls_get_address(void *control)
{
    uintptr_t self;
    __asm__ volatile ("movq %%fs:16, %0" : "=r"(self));
    if (!self)
        return __real___emutls_get_address(control);

    unsigned row = (unsigned)((self >> 4) ^ (self >> 14)) & (EMUTLS_ROWS - 1u);
    unsigned slot = control_slot(control);
    if (__atomic_load_n(&s_owners[row], __ATOMIC_RELAXED) == self) {
        const EmutlsCell *cells = s_cells[row];
        if (cells[slot].control == control)
            return cells[slot].ptr;
        if (cells[slot ^ 1u].control == control)
            return cells[slot ^ 1u].ptr;
    }
    return cache_miss(control, self, row, slot);
}

