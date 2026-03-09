/*
 * include/kernel/kai_dag.h — KAI Kernel IR: The Graph Container
 *
 * kai_dag_t manages a collection of kai_node_t pointers forming a directed
 * acyclic computation graph (pipeline). It is the graph layer between the
 * AIQL AST and the scheduler.
 *
 * Responsibilities:
 *   - Store a set of node pointers (no ownership — interner owns nodes)
 *   - Track the root (output) node
 *   - Expose graph-level operations (add, remove, validate)
 *   - NOT responsible for execution — that belongs to kai_scheduler
 *
 * Layering:
 *   AIQL AST  →  kai_dag_build_from_pipeline()  →  kai_dag_t
 *                                                        ↓
 *                                               kai_scheduler_build()
 *                                                        ↓
 *                                               kai_schedule_t (stages)
 *                                                        ↓
 *                                               sandbox interpreter (exec)
 */

#ifndef KERNEL_KAI_DAG_H
#define KERNEL_KAI_DAG_H

#include <kernel/kai_node.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Limits -------------------------------------------------------------- */
#define KAI_DAG_MAX_NODES   64U   /* Max nodes in a single DAG */

/* ---- Result codes -------------------------------------------------------- */
typedef enum {
    KAI_DAG_OK          = 0,
    KAI_DAG_ERR_FULL    = 1,   /* Node capacity exceeded    */
    KAI_DAG_ERR_CYCLE   = 2,   /* Cycle detected in graph   */
    KAI_DAG_ERR_NULL    = 3,   /* NULL argument             */
    KAI_DAG_ERR_DUP     = 4,   /* Node already in DAG       */
} kai_dag_result_t;

/*
 * kai_dag_t — The graph container.
 *
 * nodes[] holds pointers to nodes in insertion order. The interner owns the
 * actual node memory; the DAG only holds references.
 *
 * root is the terminal node whose result is the pipeline output.
 * It is the only node with no outgoing edges (no other node depends on it).
 */
typedef struct {
    kai_node_t  *nodes[KAI_DAG_MAX_NODES];
    uint32_t     node_count;
    kai_node_t  *root;
} kai_dag_t;

/*
 * kai_dag_init — Zero-initialize a DAG structure.
 */
void kai_dag_init(kai_dag_t *dag);

/*
 * kai_dag_add_node — Add a node pointer to the DAG.
 *
 * Does not transfer ownership. The node must remain valid for the DAG's
 * lifetime. Returns KAI_DAG_ERR_DUP if the node is already present.
 */
kai_dag_result_t kai_dag_add_node(kai_dag_t *dag, kai_node_t *node);

/*
 * kai_dag_set_root — Designate the DAG's terminal (output) node.
 *
 * The root node must already be present in the DAG's node set.
 * Returns false if the node is not found.
 */
bool kai_dag_set_root(kai_dag_t *dag, kai_node_t *node);

/*
 * kai_dag_build_from_pipeline — Construct a DAG from an existing pipeline_t.
 *
 * Maps each pipeline step to a kai_node, wires sequential dependencies,
 * and sets the last step as root. Uses the provided interner for
 * node deduplication.
 *
 * Returns true on success. The dag and interner are modified in place.
 */
struct kai_interner;
bool kai_dag_build_from_pipeline(kai_dag_t *dag,
                                  struct kai_interner *intern,
                                  const pipeline_t *pipeline);

/*
 * kai_dag_has_cycle — Cycle detection using iterative DFS.
 *
 * Returns true if a cycle exists (which would make the graph invalid).
 * A valid DAG must return false before scheduling.
 */
bool kai_dag_has_cycle(const kai_dag_t *dag);

/*
 * kai_dag_destroy — Release all node references.
 *
 * Calls kai_node_release on every node in the DAG and zeros the structure.
 * Does not free node memory (the interner owns it).
 */
void kai_dag_destroy(kai_dag_t *dag);

#endif /* KERNEL_KAI_DAG_H */