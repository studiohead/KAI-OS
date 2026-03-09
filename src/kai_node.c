/*
 * src/kai_node.c — KAI Kernel IR: Node hash and equality
 *
 * Provides the two primitive operations that make interning work:
 *   kai_node_hash  — structural hash (FNV-1a) over opcode + args + dep hashes
 *   kai_node_equal — deep structural equality check
 *
 * Both functions treat node identity as purely structural: two nodes are
 * "the same" if they compute the same thing from the same inputs. The hash
 * mixes in dependency hashes so that graph depth is captured — two nodes
 * with identical opcodes but different dependency chains get distinct hashes.
 */

#include <kernel/kai_node.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- FNV-1a constants (32-bit) ------------------------------------------ */
#define FNV_OFFSET_BASIS  2166136261U
#define FNV_PRIME         16777619U

/* Mix one byte into a running FNV-1a hash. */
static inline uint32_t fnv_mix_byte(uint32_t h, uint8_t b)
{
    return (h ^ (uint32_t)b) * FNV_PRIME;
}

/* Mix a 32-bit word into the hash (little-endian byte order). */
static inline uint32_t fnv_mix_u32(uint32_t h, uint32_t v)
{
    h = fnv_mix_byte(h, (uint8_t)(v        ));
    h = fnv_mix_byte(h, (uint8_t)(v >>  8U ));
    h = fnv_mix_byte(h, (uint8_t)(v >> 16U ));
    h = fnv_mix_byte(h, (uint8_t)(v >> 24U ));
    return h;
}

/* Mix a NUL-terminated string into the hash. */
static inline uint32_t fnv_mix_str(uint32_t h, const char *s)
{
    while (*s != '\0') {
        h = fnv_mix_byte(h, (uint8_t)*s);
        s++;
    }
    /* NUL terminator as separator, so "ab"+"c" != "a"+"bc" */
    h = fnv_mix_byte(h, 0U);
    return h;
}

/* ======================================================================
 * kai_node_hash
 *
 * Structural hash over:
 *   1. opcode (uint32_t)
 *   2. argc   (uint32_t)
 *   3. Each arg string (with NUL separators)
 *   4. dep_count (uint32_t)
 *   5. Each dep's existing hash (mixes graph topology into this node's id)
 *
 * Mixing dep hashes rather than dep pointers makes the hash content-
 * addressable: the same logical subgraph produces the same hash regardless
 * of which interner pool slot the node lives in.
 * ====================================================================== */
uint32_t kai_node_hash(sandbox_opcode_t      opcode,
                        uint32_t              argc,
                        const char            args[][KAI_NODE_ARG_LEN],
                        kai_node_t *const    *deps,
                        uint32_t              dep_count)
{
    uint32_t h = FNV_OFFSET_BASIS;

    /* Opcode */
    h = fnv_mix_u32(h, (uint32_t)opcode);

    /* Argument count + each arg string */
    h = fnv_mix_u32(h, argc);
    for (uint32_t i = 0; i < argc && i < KAI_NODE_MAX_ARGS; i++) {
        h = fnv_mix_str(h, args[i]);
    }

    /* Dependency count + each dep's structural hash */
    h = fnv_mix_u32(h, dep_count);
    for (uint32_t i = 0; i < dep_count && i < KAI_NODE_MAX_DEPS; i++) {
        if (deps[i] != (void *)0) {
            h = fnv_mix_u32(h, deps[i]->hash);
        }
    }

    return h;
}

/* ======================================================================
 * kai_node_equal
 *
 * Deep structural equality. Returns true iff:
 *   - Same opcode
 *   - Same caps_required
 *   - Same argc
 *   - All arg strings compare equal
 *   - Same dep_count
 *   - All dep pointers are identical (pointer equality is sufficient here
 *     because interned nodes are canonical — same content → same pointer)
 * ====================================================================== */
bool kai_node_equal(const kai_node_t *a, const kai_node_t *b)
{
    if (a == b)   return true;   /* Pointer shortcut — same node */
    if (!a || !b) return false;

    if (a->opcode        != b->opcode)        return false;
    if (a->caps_required != b->caps_required) return false;
    if (a->argc          != b->argc)          return false;
    if (a->dep_count     != b->dep_count)     return false;

    /* Compare argument strings */
    for (uint32_t i = 0; i < a->argc && i < KAI_NODE_MAX_ARGS; i++) {
        if (k_strcmp(a->args[i], b->args[i]) != 0) return false;
    }

    /* Compare dependency pointers.
     * Because all nodes are interned, pointer equality ↔ structural equality. */
    for (uint32_t i = 0; i < a->dep_count && i < KAI_NODE_MAX_DEPS; i++) {
        if (a->deps[i] != b->deps[i]) return false;
    }

    return true;
}