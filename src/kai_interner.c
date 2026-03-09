/*
 * src/kai_interner.c — KAI Kernel IR: Canonical Node Factory
 *
 * The interner prevents duplicate nodes and transforms the runtime graph from
 * a tree into a true DAG: structurally identical nodes collapse into one
 * shared canonical pointer, enabling safe common-subexpression elimination.
 *
 * Implementation details:
 *   - Static pool[] of KAI_INTERNER_POOL_SIZE nodes (no malloc on bare metal)
 *   - Open-addressing hash table with linear probing over bins[]
 *   - bins[i] stores (pool_index + 1), 0 means empty slot
 *   - Hash collision resolved by kai_node_equal() for structural comparison
 *   - Once a node is stored it is marked KAI_NODE_IMMUTABLE
 *
 * Thread / interrupt safety:
 *   Not reentrant. On this bare-metal system the interner is used only from
 *   the main shell context, not from the IRQ handler. If that changes, add
 *   a critical section guard around get_or_create.
 */

#include <kernel/kai_interner.h>
#include <kernel/kai_node.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Internal helpers ---------------------------------------------------- */

static void safe_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1U && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Map a 32-bit hash to a bin index. BINS must be a power of 2. */
static inline uint32_t hash_to_bin(uint32_t h)
{
    return h & (KAI_INTERNER_HASH_BINS - 1U);
}

/* ======================================================================
 * kai_interner_init
 * ====================================================================== */
void kai_interner_init(kai_interner_t *intern)
{
    if (!intern) return;
    k_memset(intern, 0, sizeof(kai_interner_t));
}

/* ======================================================================
 * kai_node_retain / kai_node_release
 *
 * Reference counting for shared node ownership. On bare metal we never
 * free pool slots, but tracking ref_count lets diagnostics detect leaks
 * and unexpected releases.
 * ====================================================================== */
void kai_node_retain(kai_node_t *node)
{
    if (node) node->ref_count++;
}

void kai_node_release(kai_node_t *node)
{
    if (node && node->ref_count > 0U) node->ref_count--;
}

/* ======================================================================
 * kai_interner_get_or_create — Core interning operation
 *
 * Steps:
 *   1. Compute structural hash from (opcode, args, dep hashes).
 *   2. Probe bins[] starting at hash % BINS with linear probing.
 *   3a. If an occupied bin's node passes kai_node_equal(), it's a hit:
 *       retain + return it.
 *   3b. If all probed bins are occupied by non-matching nodes (unlikely
 *       given low load), or if the table is full, return NULL.
 *   3c. If an empty bin is found, allocate a new pool slot, fill it,
 *       mark it KAI_NODE_IMMUTABLE, store index+1 in the bin, return it.
 * ====================================================================== */
kai_node_t *kai_interner_get_or_create(kai_interner_t   *intern,
                                        sandbox_opcode_t  opcode,
                                        uint32_t          caps_required,
                                        uint32_t          argc,
                                        const char        args[][KAI_NODE_ARG_LEN],
                                        kai_node_t       **deps,
                                        uint32_t          dep_count,
                                        uint32_t          cost_estimate)
{
    if (!intern || !args) return (void *)0;
    if (dep_count > KAI_NODE_MAX_DEPS) return (void *)0;

    /* Step 1: Compute structural hash */
    uint32_t h = kai_node_hash(opcode, argc, args, deps, dep_count);

    /* Step 2: Probe hash table */
    uint32_t bin      = hash_to_bin(h);
    uint32_t probes   = 0;
    int32_t  free_bin = -1;   /* First empty bin found during probe */

    while (probes < KAI_INTERNER_HASH_BINS) {
        uint8_t slot = intern->bins[bin];

        if (slot == 0U) {
            /* Empty bin — record it and stop probing (open addressing) */
            if (free_bin < 0) free_bin = (int32_t)bin;
            break;
        }

        /* Occupied — check for structural match */
        kai_node_t *candidate = &intern->pool[slot - 1U];
        if (candidate->hash == h && kai_node_equal(candidate,
                /* build a temporary view for comparison */
                &(kai_node_t){
                    .opcode        = opcode,
                    .caps_required = caps_required,
                    .argc          = argc,
                    .dep_count     = dep_count,
                }))
        {
            /* Full structural match — this node is already interned */
            kai_node_retain(candidate);
            return candidate;
        }

        /* Hash collision — advance (linear probe) */
        bin    = hash_to_bin(bin + 1U);
        probes++;
    }

    /* Step 3c: New node — allocate from pool */
    if (free_bin < 0) return (void *)0;           /* Table full */
    if (intern->pool_used >= KAI_INTERNER_POOL_SIZE) return (void *)0;

    uint32_t    pool_idx = intern->pool_used++;
    kai_node_t *node     = &intern->pool[pool_idx];

    k_memset(node, 0, sizeof(kai_node_t));

    node->opcode        = opcode;
    node->caps_required = caps_required;
    node->argc          = (argc < KAI_NODE_MAX_ARGS) ? argc : KAI_NODE_MAX_ARGS;
    node->dep_count     = (dep_count < KAI_NODE_MAX_DEPS) ? dep_count : KAI_NODE_MAX_DEPS;
    node->hash          = h;
    node->ref_count     = 1U;
    node->cost_estimate = cost_estimate;
    node->flags         = KAI_NODE_IMMUTABLE;

    /* Copy argument strings */
    for (uint32_t i = 0; i < node->argc; i++) {
        safe_strcpy(node->args[i], args[i], KAI_NODE_ARG_LEN);
    }

    /* Copy dependency pointers and retain each dep */
    for (uint32_t i = 0; i < node->dep_count; i++) {
        node->deps[i] = deps[i];
        if (deps[i]) kai_node_retain(deps[i]);
    }

    /* Register in hash table */
    intern->bins[(uint32_t)free_bin] = (uint8_t)(pool_idx + 1U);

    return node;
}

/* ======================================================================
 * kai_interner_stats — Print pool usage to UART
 *
 * Output format:
 *   interner: N/64 nodes  (M bins used)
 *   [idx] opcode=N hash=0x... refs=N cost=N deps=N
 * ====================================================================== */
void kai_interner_stats(const kai_interner_t *intern, uint32_t caps)
{
    if (!intern) return;

    static const char header[] = "interner pool:\r\n";
    sys_uart_write(header, sizeof(header) - 1U, caps);

    for (uint32_t i = 0; i < intern->pool_used; i++) {
        const kai_node_t *n = &intern->pool[i];

        /* Print index */
        sys_uart_write("  [", 3, caps);
        char idx_c = (char)('0' + (int)(i % 10U));
        sys_uart_write(&idx_c, 1, caps);
        sys_uart_write("] op=", 5, caps);
        sys_uart_hex64((uint64_t)n->opcode, caps);
        sys_uart_write(" hash=", 6, caps);
        sys_uart_hex64((uint64_t)n->hash, caps);
        sys_uart_write(" refs=", 6, caps);
        sys_uart_hex64((uint64_t)n->ref_count, caps);
        sys_uart_write(" cost=", 6, caps);
        sys_uart_hex64((uint64_t)n->cost_estimate, caps);
        sys_uart_write(" deps=", 6, caps);
        sys_uart_hex64((uint64_t)n->dep_count, caps);
        sys_uart_write("\r\n", 2, caps);
    }
}