/*
 * src/kai_scheduler.c — KAI Kernel IR: Execution Planner
 *
 * Transforms a kai_dag_t into a kai_schedule_t: an ordered sequence of
 * kai_stage_t groups. Nodes within a stage are independent and can execute
 * in parallel (or be batched). Stages execute sequentially.
 *
 * Algorithm: Level-based topological sort
 * ─────────────────────────────────────
 * For each node N in the DAG:
 *   level[N] = max(level[dep] for dep in N.deps) + 1
 *              (nodes with no deps → level 0)
 *
 * Nodes at the same level form one stage. Within a stage, nodes are sorted
 * by cost_estimate ascending (cheap-first): run OP_ECHO/OP_EL before
 * OP_READ, run OP_READ before any hypothetical LLM call.
 *
 * Why cheap-first?
 *   - Cheap nodes (UART, EL queries) complete instantly and often set up
 *     variables that expensive nodes need — completing them early avoids
 *     idle time in later stages.
 *   - It mirrors SQL query planner logic: push cheap filters early, defer
 *     full-table scans and expensive joins.
 *   - For AIQL: FeatureEngineering (OP ~10) runs before classifier (~100)
 *     which runs before llm (~1000), matching the AIQL pipeline structure.
 *
 * Example — 4-node linear chain:
 *   echo → read → caps → el
 *   Level: 0       1      2    3
 *   Stage 0: [echo]
 *   Stage 1: [read]
 *   Stage 2: [caps]
 *   Stage 3: [el]
 *
 * Example — diamond DAG (A→B, A→C, B→D, C→D):
 *   Stage 0: [A]          ← source, no deps
 *   Stage 1: [B, C]       ← both depend only on A; parallel!
 *   Stage 2: [D]          ← depends on B and C; runs after stage 1
 *
 * This is where parallelism appears naturally from the graph structure.
 */

#include <kernel/kai_scheduler.h>
#include <kernel/kai_dag.h>
#include <kernel/kai_node.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Internal: level computation ---------------------------------------- */

/*
 * compute_levels — Assign each node a topological level.
 *
 * Uses Kahn's algorithm (in-degree counting) adapted for pointer-based
 * adjacency. We iterate until no new level changes occur (fixed-point).
 * On a valid DAG this converges in O(depth) passes.
 *
 * levels[] is indexed by position in dag->nodes[].
 * Returns false if a cycle prevents convergence (shouldn't happen if
 * kai_dag_has_cycle was already checked, but guards against corruption).
 */
static bool compute_levels(const kai_dag_t *dag,
                            uint32_t         levels[KAI_DAG_MAX_NODES],
                            uint32_t        *max_level_out)
{
    k_memset(levels, 0, KAI_DAG_MAX_NODES * sizeof(uint32_t));

    uint32_t max_level = 0;
    bool     changed   = true;
    uint32_t passes    = 0;

    /* Fixed-point: keep updating until no level changes */
    while (changed && passes < KAI_DAG_MAX_NODES) {
        changed = false;
        passes++;

        for (uint32_t i = 0; i < dag->node_count; i++) {
            kai_node_t *node = dag->nodes[i];

            for (uint32_t d = 0; d < node->dep_count; d++) {
                kai_node_t *dep = node->deps[d];
                if (!dep) continue;

                /* Find dep's position in the DAG node list */
                int32_t dep_pos = -1;
                for (uint32_t j = 0; j < dag->node_count; j++) {
                    if (dag->nodes[j] == dep) { dep_pos = (int32_t)j; break; }
                }
                if (dep_pos < 0) continue;

                /* level[node] must be at least level[dep] + 1 */
                uint32_t required = levels[(uint32_t)dep_pos] + 1U;
                if (required > levels[i]) {
                    levels[i] = required;
                    if (levels[i] > max_level) max_level = levels[i];
                    changed = true;
                }
            }
        }
    }

    if (changed) return false;   /* Did not converge — cycle present */

    *max_level_out = max_level;
    return true;
}

/* ---- Internal: insertion sort within a stage (cheap-first) -------------- */
/*
 * sort_stage_by_cost — Sort stage->nodes[] ascending by cost_estimate.
 *
 * Insertion sort: O(n²) but n ≤ KAI_STAGE_MAX_NODES (16), so this is fine
 * on bare metal where we want predictable, allocation-free sorting.
 */
static void sort_stage_by_cost(kai_stage_t *stage)
{
    for (uint32_t i = 1; i < stage->node_count; i++) {
        kai_node_t *key  = stage->nodes[i];
        int32_t     j    = (int32_t)i - 1;

        while (j >= 0 && stage->nodes[j]->cost_estimate > key->cost_estimate) {
            stage->nodes[j + 1] = stage->nodes[j];
            j--;
        }
        stage->nodes[j + 1] = key;
    }
}

/* ======================================================================
 * kai_scheduler_build — Build the execution plan
 * ====================================================================== */
kai_sched_result_t kai_scheduler_build(const kai_dag_t *dag,
                                        kai_schedule_t  *out)
{
    if (!dag || !out) return KAI_SCHED_ERR_NULL;
    if (dag->node_count == 0) return KAI_SCHED_ERR_EMPTY;

    k_memset(out, 0, sizeof(kai_schedule_t));

    /* Step 1: Compute topological levels */
    uint32_t levels[KAI_DAG_MAX_NODES];
    uint32_t max_level = 0;

    if (!compute_levels(dag, levels, &max_level)) {
        return KAI_SCHED_ERR_CYCLE;
    }

    uint32_t num_stages = max_level + 1U;
    if (num_stages > KAI_MAX_STAGES) return KAI_SCHED_ERR_OVERFLOW;

    /* Step 2: Initialise stage metadata */
    for (uint32_t s = 0; s < num_stages; s++) {
        out->stages[s].stage_level  = s;
        out->stages[s].node_count   = 0;
        out->stages[s].total_cost   = 0;
    }
    out->stage_count  = num_stages;
    out->total_nodes  = 0;

    /* Step 3: Place each node into its stage */
    for (uint32_t i = 0; i < dag->node_count; i++) {
        uint32_t     lvl   = levels[i];
        kai_stage_t *stage = &out->stages[lvl];

        if (stage->node_count >= KAI_STAGE_MAX_NODES) {
            return KAI_SCHED_ERR_OVERFLOW;
        }

        stage->nodes[stage->node_count++] = dag->nodes[i];
        stage->total_cost += dag->nodes[i]->cost_estimate;
        out->total_nodes++;
    }

    /* Step 4: Sort each stage cheap-first */
    for (uint32_t s = 0; s < num_stages; s++) {
        sort_stage_by_cost(&out->stages[s]);
    }

    return KAI_SCHED_OK;
}

/* ======================================================================
 * kai_scheduler_print — Dump the schedule to UART
 *
 * Example output:
 *   schedule: 3 stages, 4 nodes
 *   stage 0 [cost=1]:
 *     [0] op=4 cost=1  (OP_ECHO)
 *   stage 1 [cost=10]:
 *     [0] op=1 cost=10  (OP_READ)
 *   stage 2 [cost=1]:
 *     [0] op=6 cost=1  (OP_CAPS)
 * ====================================================================== */
void kai_scheduler_print(const kai_schedule_t *schedule, uint32_t caps)
{
    if (!schedule) return;

    sys_uart_write("schedule: ", 10, caps);
    sys_uart_hex64((uint64_t)schedule->stage_count, caps);
    sys_uart_write(" stages, ", 9, caps);
    sys_uart_hex64((uint64_t)schedule->total_nodes, caps);
    sys_uart_write(" nodes\r\n", 8, caps);

    for (uint32_t s = 0; s < schedule->stage_count; s++) {
        const kai_stage_t *stage = &schedule->stages[s];

        sys_uart_write("  stage ", 8, caps);
        sys_uart_hex64((uint64_t)s, caps);
        sys_uart_write(" [cost=", 7, caps);
        sys_uart_hex64((uint64_t)stage->total_cost, caps);
        sys_uart_write("] ", 2, caps);
        sys_uart_hex64((uint64_t)stage->node_count, caps);
        sys_uart_write(" node(s):\r\n", 11, caps);

        for (uint32_t n = 0; n < stage->node_count; n++) {
            const kai_node_t *node = stage->nodes[n];
            sys_uart_write("    op=", 7, caps);
            sys_uart_hex64((uint64_t)node->opcode, caps);
            sys_uart_write(" cost=", 6, caps);
            sys_uart_hex64((uint64_t)node->cost_estimate, caps);
            sys_uart_write(" hash=", 6, caps);
            sys_uart_hex64((uint64_t)node->hash, caps);
            sys_uart_write(" deps=", 6, caps);
            sys_uart_hex64((uint64_t)node->dep_count, caps);
            sys_uart_write("\r\n", 2, caps);
        }
    }
}

/* ======================================================================
 * kai_sched_result_str
 * ====================================================================== */
const char *kai_sched_result_str(kai_sched_result_t result)
{
    switch (result) {
        case KAI_SCHED_OK:           return "ok";
        case KAI_SCHED_ERR_CYCLE:    return "cycle detected";
        case KAI_SCHED_ERR_OVERFLOW: return "capacity overflow";
        case KAI_SCHED_ERR_NULL:     return "null argument";
        case KAI_SCHED_ERR_EMPTY:    return "empty dag";
        default:                     return "unknown error";
    }
}