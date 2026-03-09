/*
 * src/kai_dag.c — KAI Kernel IR: The Graph Container
 *
 * Manages a collection of kai_node_t pointers forming a directed acyclic
 * computation graph. Sits between the AIQL pipeline and the scheduler.
 *
 * The DAG does not own node memory — the interner does. The DAG only
 * holds references (pointers) and increments/decrements ref_counts
 * on add/destroy via the retain/release API.
 *
 * AIQL mapping:
 *   PipelineStatement  → one kai_dag_t per pipeline
 *   Operation step     → low-cost kai_node_t (cost ~10)
 *   CallStatement/llm  → high-cost kai_node_t (cost ~1000)
 *   CallStatement/cls  → medium-cost kai_node_t (cost ~100)
 *   ConditionalStmt    → OP_IF kai_node_t with two dep chains
 *   Sequential steps   → linear dependency chain via deps[]
 *
 * Cycle detection uses iterative DFS with a colour array (WHITE/GREY/BLACK).
 */

#include <kernel/kai_dag.h>
#include <kernel/kai_interner.h>
#include <kernel/syscall.h>
#include <kernel/string.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Cycle detection colours -------------------------------------------- */
#define CLR_WHITE  0U   /* Not yet visited                */
#define CLR_GREY   1U   /* In current DFS stack (active)  */
#define CLR_BLACK  2U   /* Fully explored (no cycle)      */

/* ---- AIQL-aware cost estimates ------------------------------------------ */
/*
 * These mirror the AIQL call_type hierarchy:
 *   llm calls        → KAI_COST_LLM    (expensive; defer, batch)
 *   classifier calls → KAI_COST_CLS    (medium; GPU-bound)
 *   function calls   → KAI_COST_FN     (cheap; CPU-bound)
 *   Operations       → KAI_COST_OP     (very cheap; local transform)
 *   Control / IO     → KAI_COST_CTRL   (near-zero; UART, EL query)
 *
 * The scheduler uses these to sort within a stage: cheap nodes first,
 * expensive LLM nodes last. This mirrors SQL query planner cost ordering.
 */
#define KAI_COST_LLM   1000U
#define KAI_COST_CLS    100U
#define KAI_COST_FN      50U
#define KAI_COST_OP      10U
#define KAI_COST_CTRL     1U

/* ---- Internal helpers ---------------------------------------------------- */

/* Return the cost estimate for a given opcode. Used during build_from_pipeline. */
static uint32_t default_cost(sandbox_opcode_t op)
{
    switch (op) {
        /* IO / control — nearly free */
        case OP_NOP:
        case OP_EL:
        case OP_CAPS:
        case OP_WAIT_EVENT:
            return KAI_COST_CTRL;

        /* UART output — cheap */
        case OP_ECHO:
        case OP_INTROSPECT:
            return KAI_COST_CTRL;

        /* Memory ops — cheap but not trivial */
        case OP_READ:
        case OP_WRITE:
        case OP_INFO:
            return KAI_COST_OP;

        /* Conditional branch — structural, not expensive */
        case OP_IF:
            return KAI_COST_CTRL;

        /* Timing — variable; treat as an operation */
        case OP_SLEEP:
            return KAI_COST_FN;

        default:
            return KAI_COST_OP;
    }
}

/* Return the index of a node in dag->nodes[], or -1 if not found. */
static int32_t find_node(const kai_dag_t *dag, const kai_node_t *node)
{
    for (uint32_t i = 0; i < dag->node_count; i++) {
        if (dag->nodes[i] == node) return (int32_t)i;
    }
    return -1;
}

/* ======================================================================
 * kai_dag_init
 * ====================================================================== */
void kai_dag_init(kai_dag_t *dag)
{
    if (!dag) return;
    k_memset(dag, 0, sizeof(kai_dag_t));
}

/* ======================================================================
 * kai_dag_add_node
 * ====================================================================== */
kai_dag_result_t kai_dag_add_node(kai_dag_t *dag, kai_node_t *node)
{
    if (!dag || !node) return KAI_DAG_ERR_NULL;
    if (dag->node_count >= KAI_DAG_MAX_NODES) return KAI_DAG_ERR_FULL;
    if (find_node(dag, node) >= 0) return KAI_DAG_ERR_DUP;

    dag->nodes[dag->node_count++] = node;
    kai_node_retain(node);
    return KAI_DAG_OK;
}

/* ======================================================================
 * kai_dag_set_root
 * ====================================================================== */
bool kai_dag_set_root(kai_dag_t *dag, kai_node_t *node)
{
    if (!dag || !node) return false;
    if (find_node(dag, node) < 0) return false;
    dag->root = node;
    return true;
}

/* ======================================================================
 * kai_dag_build_from_pipeline
 *
 * Converts a linear pipeline_t into a DAG. Each step becomes a node,
 * with a sequential dependency edge to the previous step.
 *
 * AIQL note: A real AIQL pipeline has branching (ConditionalStatement)
 * and parallel branches (independent CallStatements). The linear
 * dependency chain here is a conservative safe default — independent
 * steps with no shared outputs get no dep edges, allowing the scheduler
 * to place them in the same stage automatically.
 *
 * Linear chain: step[0] ← step[1] ← step[2] ← ... ← step[N-1] (root)
 *
 * For a true AIQL program with branching CallStatements, the planner
 * layer (future) will build a richer dep graph rather than a chain.
 * ====================================================================== */
bool kai_dag_build_from_pipeline(kai_dag_t           *dag,
                                  struct kai_interner *intern,
                                  const pipeline_t    *pipeline)
{
    if (!dag || !intern || !pipeline) return false;
    if (pipeline->step_count == 0) return false;

    kai_dag_init(dag);

    kai_node_t *prev = (void *)0;

    for (uint32_t i = 0; i < pipeline->step_count; i++) {
        const pipeline_node_t *step = &pipeline->steps[i];

        /* Build dep list: depends on previous node (if any) */
        kai_node_t *deps[1];
        uint32_t    dep_count = 0;
        if (prev) {
            deps[0]   = prev;
            dep_count = 1;
        }

        /* Derive cost from opcode */
        uint32_t cost = default_cost(step->opcode);

        /* Derive required caps from opcode (mirrors verifier's table) */
        uint32_t caps = 0U;
        switch (step->opcode) {
            case OP_READ:
            case OP_INFO:       caps = CAP_READ_MEM; break;
            case OP_WRITE:      caps = CAP_WRITE_MEM; break;
            case OP_ECHO:
            case OP_EL:
            case OP_CAPS:
            case OP_INTROSPECT: caps = CAP_MMIO; break;
            default:            caps = 0U; break;
        }

        kai_node_t *node = kai_interner_get_or_create(
            (kai_interner_t *)intern,
            step->opcode,
            caps,
            step->argc,
            (const char (*)[KAI_NODE_ARG_LEN])step->args,
            dep_count ? deps : (void *)0,
            dep_count,
            cost
        );

        if (!node) return false;

        if (kai_dag_add_node(dag, node) != KAI_DAG_OK) return false;

        prev = node;
    }

    /* Last node is the root (terminal output) */
    if (prev) kai_dag_set_root(dag, prev);

    return true;
}

/* ======================================================================
 * kai_dag_has_cycle — Iterative DFS cycle detection
 *
 * Uses explicit stacks to avoid recursion (no stack space guarantee on
 * bare metal). Tracks visitation state with a colour array.
 *
 * A cycle exists iff DFS reaches a GREY (active) node from itself.
 * ====================================================================== */
bool kai_dag_has_cycle(const kai_dag_t *dag)
{
    if (!dag || dag->node_count == 0) return false;

    /* Colour array indexed by position in dag->nodes[] */
    uint8_t colour[KAI_DAG_MAX_NODES];
    k_memset(colour, CLR_WHITE, sizeof(colour));

    /* Explicit DFS stack storing node index */
    int32_t  stk[KAI_DAG_MAX_NODES];
    uint8_t  dep_idx[KAI_DAG_MAX_NODES];   /* Which dep to visit next */
    int32_t  top = -1;

    /* Run DFS from every unvisited node (graph may be disconnected) */
    for (uint32_t start = 0; start < dag->node_count; start++) {
        if (colour[start] != CLR_WHITE) continue;

        top = 0;
        stk[0]     = (int32_t)start;
        dep_idx[0] = 0;
        colour[start] = CLR_GREY;

        while (top >= 0) {
            int32_t cur = stk[top];
            kai_node_t *node = dag->nodes[cur];

            /* Find next unvisited dep */
            bool pushed = false;
            while (dep_idx[top] < node->dep_count) {
                kai_node_t *dep = node->deps[dep_idx[top]++];
                if (!dep) continue;

                /* Find dep's index in dag->nodes[] */
                int32_t dep_pos = find_node(dag, dep);
                if (dep_pos < 0) continue;   /* Dep not in this DAG */

                if (colour[dep_pos] == CLR_GREY) return true;   /* Cycle! */
                if (colour[dep_pos] == CLR_WHITE) {
                    colour[dep_pos] = CLR_GREY;
                    top++;
                    stk[top]     = dep_pos;
                    dep_idx[top] = 0;
                    pushed = true;
                    break;
                }
            }

            if (!pushed) {
                /* All deps explored — mark done and pop */
                colour[cur] = CLR_BLACK;
                top--;
            }
        }
    }

    return false;
}

/* ======================================================================
 * kai_dag_destroy
 * ====================================================================== */
void kai_dag_destroy(kai_dag_t *dag)
{
    if (!dag) return;
    for (uint32_t i = 0; i < dag->node_count; i++) {
        if (dag->nodes[i]) {
            kai_node_release(dag->nodes[i]);
            dag->nodes[i] = (void *)0;
        }
    }
    dag->node_count = 0;
    dag->root       = (void *)0;
}