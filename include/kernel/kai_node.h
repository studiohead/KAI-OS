/*
 * include/kernel/kai_node.h — KAI Kernel IR: The Computation Atom
 *
 * kai_node_t is the immutable definition of a single computation unit in the
 * KAI runtime. It sits above the sandbox's linear pipeline model and forms
 * the vertices of a true directed acyclic graph (DAG).
 *
 * Layering:
 *   AIQL AST node
 *         ↓
 *   kai_node_t   ← you are here
 *         ↓
 *   sandbox pipeline_node_t → CPU execution
 *
 * Design principles:
 *   - Nodes are IMMUTABLE after the KAI_NODE_IMMUTABLE flag is set.
 *   - Identity is structural: opcode + caps + argc + args + dep hashes.
 *   - hash encodes identity for O(1) interner lookups.
 *   - cost_estimate drives scheduler optimization (cheap-first ordering).
 *   - ref_count enables safe sharing across multiple DAGs.
 */

#ifndef KERNEL_KAI_NODE_H
#define KERNEL_KAI_NODE_H

#include <kernel/sandbox.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Limits -------------------------------------------------------------- */
#define KAI_NODE_MAX_DEPS    8U    /* Max dependency edges per node             */
#define KAI_NODE_MAX_ARGS    4U    /* Max operands (mirrors SANDBOX_MAX_ARGS)   */
#define KAI_NODE_ARG_LEN     32U   /* Max argument string length                */

/* ---- Node flags ---------------------------------------------------------- */
#define KAI_NODE_VALIDATED   (1U << 0)  /* kai_validator has approved this node */
#define KAI_NODE_IMMUTABLE   (1U << 1)  /* Interned; structural fields are frozen */
#define KAI_NODE_SCHEDULED   (1U << 2)  /* Placed in an execution stage          */

/*
 * kai_node_t — The computation atom.
 *
 * Once immutable, all pointer-sharing and hash-keyed lookup is safe.
 * deps[] forms the edges of the DAG: node N depends on deps[0..dep_count-1],
 * meaning deps must complete before N can execute.
 *
 * cost_estimate is an arbitrary unit (higher = more expensive).
 * The scheduler uses this to sort within a stage: run cheap nodes first,
 * delay expensive LLM/IO calls, and batch similar operations.
 */
typedef struct kai_node {
    sandbox_opcode_t  opcode;
    uint32_t          hash;             /* Structural identity hash              */
    uint32_t          ref_count;        /* Shared reference count                */
    uint32_t          flags;            /* KAI_NODE_VALIDATED | IMMUTABLE | ...  */
    uint32_t          cost_estimate;    /* Scheduler hint: higher = more costly  */
    uint32_t          caps_required;    /* Capability mask needed to execute      */
    uint32_t          dep_count;        /* Number of entries in deps[]            */
    struct kai_node  *deps[KAI_NODE_MAX_DEPS];
    uint32_t          argc;
    char              args[KAI_NODE_MAX_ARGS][KAI_NODE_ARG_LEN];
} kai_node_t;

/*
 * kai_node_hash — Compute structural hash from node fields.
 *
 * Uses FNV-1a over opcode, args, and dependency hashes.
 * Dependency hashes are mixed in to make the hash tree-sensitive:
 * two nodes with the same opcode/args but different deps get different hashes.
 */
uint32_t kai_node_hash(sandbox_opcode_t opcode,
                        uint32_t argc,
                        const char args[][KAI_NODE_ARG_LEN],
                        kai_node_t *const *deps,
                        uint32_t dep_count);

/*
 * kai_node_equal — Deep structural equality check.
 *
 * Returns true iff both nodes have the same opcode, caps, argc, args, and
 * dep_count with matching dep pointers. Used by the interner to confirm
 * hash collision vs. true structural match.
 */
bool kai_node_equal(const kai_node_t *a, const kai_node_t *b);

#endif /* KERNEL_KAI_NODE_H */