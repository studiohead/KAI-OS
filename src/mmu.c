/*
 * src/mmu.c — KAI OS MMU page table initialisation
 *
 * This implementation synchronises with include/kernel/mmu.h to ensure
 * that MAIR, TCR, and PTE attributes are consistent across the kernel.
 */

#include <kernel/mmu.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Linker symbols ------------------------------------------------------- */
extern char __kernel_end[];
extern char __sandbox_scratch_start[];
extern char __page_tables_start[];
extern char __page_tables_end[];

/* ---- Static table pointers (within .page_tables section) ----------------- */
#define L1_TABLE   ((uint64_t *)(__page_tables_start + 0x0000))
#define L2_GB0     ((uint64_t *)(__page_tables_start + 0x1000))  /* 0x00000000–0x3FFFFFFF */
#define L2_GB1     ((uint64_t *)(__page_tables_start + 0x2000))  /* 0x40000000–0x7FFFFFFF */

/* ---- VA → table index (32-bit VA, T0SZ=32, 4KB granule) ----------------- */
#define L1_IDX(va)  (((va) >> 30) & 0x3UL)     /* bits [31:30] */
#define L2_IDX(va)  (((va) >> 21) & 0x1FFUL)   /* bits [29:21] */

/* ---- Descriptor helpers -------------------------------------------------- */

static inline uint64_t table_desc(uint64_t *l2_page)
{
    /* bits[1:0] = 0b11 → table descriptor */
    return ((uint64_t)(uintptr_t)l2_page) | PTE_VALID | PTE_TABLE;
}

static inline uint64_t block_desc(uint64_t pa_2mb_aligned, uint64_t attrs)
{
    /* bits[1:0] = 0b01 → block descriptor (at L2) */
    return (pa_2mb_aligned & ~0x1FFFFFULL) | attrs | PTE_VALID | PTE_BLOCK;
}

/* Forward declaration for the internal mapping helper */
static void map_2mb(uint64_t *l2, uint64_t va, uint64_t pa, uint64_t attr);

/* ======================================================================
 * mmu_init — build identity page tables
 * ====================================================================== */
void mmu_init(void)
{
    
    /* Zero the 3 pages (12KB) allocated for tables in linker script */
    volatile uint64_t *table_ptr = (volatile uint64_t *)__page_tables_start;
    for (uint32_t i = 0; i < (0x3000 / 8); i++) {
        table_ptr[i] = 0;
    }

    /* ---- L1 table: point first two entries to L2 tables -------------- */
    L1_TABLE[0] = table_desc(L2_GB0);   /* 0x00000000–0x3FFFFFFF */
    L1_TABLE[1] = table_desc(L2_GB1);   /* 0x40000000–0x7FFFFFFF */


    /* ---- MMIO in GB0 --------- */
    map_2mb(L2_GB0, 0x08000000ULL, 0x08000000ULL, PTE_DEVICE_RW);
    map_2mb(L2_GB0, 0x09000000ULL, 0x09000000ULL, PTE_DEVICE_RW);

    /* ---- Kernel in GB1 ---- */
    uint64_t kend = (uint64_t)(uintptr_t)__kernel_end;
    uint64_t scratch_base = (uint64_t)(uintptr_t)__sandbox_scratch_start;
    uint64_t scratch_2mb_aligned = scratch_base & ~0x1FFFFFULL;


    for (uint64_t va = 0x40000000ULL; va < kend; va += (1ULL << 21)) {
        map_2mb(L2_GB1, va, va, PTE_KERNEL_RWX);
    }
    map_2mb(L2_GB1, scratch_2mb_aligned, scratch_2mb_aligned, PTE_EL0_RW);

    /* Ensure all table writes are flushed to RAM before the MMU hardware reads them */
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* ======================================================================
 * mmu_enable — activate the MMU
 * ====================================================================== */
extern void mmu_enable_asm(uint64_t ttbr0, uint64_t ttbr1,
                            uint64_t mair,  uint64_t tcr);

void mmu_enable(void)
{

    /* Values pulled directly from include/kernel/mmu.h constants */
    uint64_t mair = MAIR_VALUE;
    uint64_t tcr  = TCR_VALUE;


    /* * We pass L1_TABLE as TTBR0. In a non-identity map, TTBR1 would 
     * point to a separate kernel-only table, but for now, we use a 
     * single shared table for simplicity.
     */
    mmu_enable_asm(
        (uint64_t)(uintptr_t)L1_TABLE,  /* TTBR0 — our identity map table */
        0ULL,                            /* TTBR1 — disabled via EPD1=1    */
        mair,
        tcr
    );
    
    /* * After this point, the CPU uses the page tables. If identity mapping
     * is incorrect, the PC will jump to a fault or the UART will hang.
     */
}

static void map_2mb(uint64_t *l2, uint64_t va, uint64_t pa, uint64_t attr)
{
    uint64_t idx = L2_IDX(va);
    l2[idx] = block_desc(pa, attr);
}

/* * mmu_map_page — dynamic mapping helper for future L3 use.
 */
bool mmu_map_page(uint64_t *ttbr, uint64_t va, uint64_t pa, uint64_t attrs)
{
    if (!ttbr) return false;
    uint64_t l1_idx = L1_IDX(va);
    
    /* Ensure the L1 entry points to a valid L2 table */
    if (!(ttbr[l1_idx] & PTE_VALID)) return false; 
    
    uint64_t *l2 = (uint64_t *)(uintptr_t)(ttbr[l1_idx] & ~0xFFFULL);
    uint64_t l2_idx = L2_IDX(va);
    
    /* Map as a 2MB block */
    l2[l2_idx] = block_desc(pa, attrs);
    
    /* Flush the change to memory */
    __asm__ volatile ("dsb ishst" ::: "memory");
    return true;
}