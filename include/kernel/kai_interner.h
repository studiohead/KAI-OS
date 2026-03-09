/*
 * include/kernel/kai_interner.h — KAI Kernel IR: Canonical Node Factory
 *
 * The interner is what makes the graph a DAG (not a tree).
 *
 * Without interning, two pipeline steps with identical opcode/args would
 * produce two separate nodes, and the graph degenerates into a tree with
 * duplicate computation. With interning, structurally equal nodes collapse
 * into one canonical pointer, shared across all DAGs.
 *
 * This enables:
 *   - Automatic common-subexpression elimination
 *   - Safe pointer-equality checks for structural identity
 *   - Reference-counted memory management for shared nodes
 *   - O(1) hash-keyed lookup (amortised)
 *
 * Layering:
 *   Any caller that needs a node should go through:
 *     kai_interner_get_or_create() → returns canonical kai_node_t*
 *   instead of allocating node structs directly.
 *
 * Memory model:
 *   The interner owns all node memory via its internal pool[].
 *   Nodes are never freed individually — the interner holds them for its
 *   lifetime. This is safe on bare metal where lifetimes are static.
 */

#ifndef KERNEL_KAI_INTERNER_H
#define KERNEL_KAI_INTERNER_H

#include <kernel/kai_node.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Limits -------------------------------------------------------------- */
#define KAI_INTERNER_POOL_SIZE   64U   /* Max unique nodes across all DAGs     */
#define KAI_INTERNER_HASH_BINS   32U   /* Hash table width (must be power of 2) */

/*
 * kai_interner_t — The canonical node factory.
 *
 * pool[]  holds all node structs. The interner owns this memory.
 * bins[]  is a flat hash table. Each bin stores an index+1 into pool[]
 *         (0 = empty slot). Collisions are resolved by linear probing.
 */
typedef struct kai_interner {
    kai_node_t pool[KAI_INTERNER_POOL_SIZE];
    uint32_t   pool_used;

    /* Hash table: bins[hash % BINS] = pool_index + 1, or 0 if empty */
    uint8_t    bins[KAI_INTERNER_HASH_BINS];
} kai_interner_t;

/*
 * kai_interner_init — Zero-initialize an interner.
 *
 * Must be called before any get_or_create calls.
 */
void kai_interner_init(kai_interner_t *intern);

/*
 * kai_interner_get_or_create — The core interning operation.
 *
 * Looks up a node with matching structural identity (opcode, caps, argc,
 * args, deps). If found, increments its ref_count and returns it.
 * If not found, allocates a new node from pool[], initialises it, marks
 * it immutable, and returns it.
 *
 * Returns NULL if the pool is full or arguments are invalid.
 *
 * Parameters:
 *   intern        — the interner to use
 *   opcode        — sandbox opcode for this node
 *   caps_required — capability mask the node requires
 *   argc          — number of string arguments
 *   args          — argument strings (argc entries, each KAI_NODE_ARG_LEN max)
 *   deps          — dependency node pointers (dep_count entries)
 *   dep_count     — number of dependency edges
 *   cost_estimate — scheduler hint (higher = more expensive)
 */
kai_node_t *kai_interner_get_or_create(kai_interner_t   *intern,
                                        sandbox_opcode_t  opcode,
                                        uint32_t          caps_required,
                                        uint32_t          argc,
                                        const char        args[][KAI_NODE_ARG_LEN],
                                        kai_node_t       **deps,
                                        uint32_t          dep_count,
                                        uint32_t          cost_estimate);

/*
 * kai_node_retain — Increment reference count.
 *
 * Call when a DAG or other structure takes a reference to a node.
 */
void kai_node_retain(kai_node_t *node);

/*
 * kai_node_release — Decrement reference count.
 *
 * On bare metal, this does not free memory (pool is static), but it
 * tracks active references for diagnostic purposes.
 */
void kai_node_release(kai_node_t *node);

/*
 * kai_interner_stats — Print pool usage to UART for introspection.
 *
 * Useful for the 'dag' shell command to show memory pressure.
 */
void kai_interner_stats(const kai_interner_t *intern, uint32_t caps);

#endif /* KERNEL_KAI_INTERNER_H */