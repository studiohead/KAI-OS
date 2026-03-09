/*
 * include/kernel/kai_scheduler.h — KAI Kernel IR: Execution Planner
 *
 * The scheduler transforms a kai_dag_t into an ordered sequence of
 * kai_stage_t groups. Nodes within a stage have no dependencies on each
 * other and can therefore execute in parallel (or be batched together).
 *
 * Algorithm: Level-based topological sort
 *   1. For each node N: level[N] = max(level[dep] for dep in N.deps) + 1
 *      (nodes with no deps get level 0)
 *   2. Group nodes by level → each level becomes one kai_stage_t
 *   3. Within each stage, sort nodes by cost_estimate ascending:
 *      run cheap nodes first, delay expensive LLM/IO calls
 *
 * This is exactly how SQL query planners, compilers, and distributed
 * schedulers optimize work graphs.
 *
 * Execution:
 *   for stage in schedule.stages:
 *       for node in stage.nodes:
 *           dispatch(node)   // nodes are independent; can be parallelized
 *       wait_for_stage()
 *
 * Layering:
 *   kai_dag_t  →  kai_scheduler_build()  →  kai_schedule_t
 *                                                  ↓
 *                                     kai_executor_run() (future)
 *                                     or sandbox interpreter (current)
 */

#ifndef KERNEL_KAI_SCHEDULER_H
#define KERNEL_KAI_SCHEDULER_H

#include <kernel/kai_node.h>
#include <kernel/kai_dag.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Limits -------------------------------------------------------------- */
#define KAI_MAX_STAGES        16U   /* Max parallel execution stages per DAG  */
#define KAI_STAGE_MAX_NODES   16U   /* Max nodes in a single stage            */

/* ---- Stage --------------------------------------------------------------- */
/*
 * kai_stage_t — A group of nodes that can execute in parallel.
 *
 * All nodes in a stage have completed all of their dependencies
 * in prior stages. Within a stage, nodes are sorted by cost_estimate
 * ascending (cheapest first).
 */
typedef struct {
    kai_node_t *nodes[KAI_STAGE_MAX_NODES];
    uint32_t    node_count;
    uint32_t    stage_level;    /* 0 = source nodes, increases toward root */
    uint32_t    total_cost;     /* sum of node cost_estimates in this stage */
} kai_stage_t;

/* ---- Schedule ------------------------------------------------------------ */
/*
 * kai_schedule_t — The full execution plan for a DAG.
 *
 * stages[0] contains all source nodes (no deps).
 * stages[stage_count-1] contains the root node.
 * Execution proceeds from stages[0] to stages[stage_count-1].
 */
typedef struct {
    kai_stage_t stages[KAI_MAX_STAGES];
    uint32_t    stage_count;
    uint32_t    total_nodes;   /* Sanity check: must equal dag->node_count */
} kai_schedule_t;

/* ---- Result codes -------------------------------------------------------- */
typedef enum {
    KAI_SCHED_OK            = 0,
    KAI_SCHED_ERR_CYCLE     = 1,   /* DAG contains a cycle — cannot schedule */
    KAI_SCHED_ERR_OVERFLOW  = 2,   /* Stage or schedule capacity exceeded     */
    KAI_SCHED_ERR_NULL      = 3,   /* NULL argument                           */
    KAI_SCHED_ERR_EMPTY     = 4,   /* DAG has no nodes                        */
} kai_sched_result_t;

/*
 * kai_scheduler_build — Build an execution plan from a validated DAG.
 *
 * Performs the level-based topological sort and fills out the kai_schedule_t.
 * The DAG must be cycle-free (call kai_dag_has_cycle first, or trust the
 * builder). The schedule does not take ownership of any nodes.
 *
 * Returns KAI_SCHED_OK on success, or an error code describing the failure.
 */
kai_sched_result_t kai_scheduler_build(const kai_dag_t *dag,
                                        kai_schedule_t  *out);

/*
 * kai_scheduler_print — Dump the schedule to UART for inspection.
 *
 * Prints each stage's level, node count, opcodes, and cost_estimates.
 * Useful for the 'dag' shell command and debugging.
 */
void kai_scheduler_print(const kai_schedule_t *schedule, uint32_t caps);

/*
 * kai_sched_result_str — Human-readable result code string.
 */
const char *kai_sched_result_str(kai_sched_result_t result);

#endif /* KERNEL_KAI_SCHEDULER_H */