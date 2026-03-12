/*
 * src/sandbox/interpreter.c — KAI OS sandbox interpreter
 *
 * Handles both single-shot and pipeline execution:
 *
 *   interpreter_parse          — tokenise one tool call into ast_node_t
 *   interpreter_exec           — execute one verified ast_node_t
 *   interpreter_parse_pipeline — parse semicolon-separated steps into pipeline_t
 *                                Handles "-> varname" output bindings
 *                                Handles "if <cond> { ... } else { ... }" blocks
 *   interpreter_exec_pipeline  — execute a verified pipeline_t step by step
 *                                with variable store, OP_IF branching, and
 *                                instruction count across the whole pipeline
 *
 * Variable store helpers:
 *   var_store_set — bind a name to a uint64_t value
 *   var_store_get — look up a name, return false if not found
 *
 * The interpreter never re-verifies. Callers must run verifier_check /
 * verifier_check_pipeline before calling exec functions.
 */

#include <kernel/sandbox.h>
#include <kernel/intent.h>
#include <kernel/syscall.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- Opcode name lookup table ------------------------------------------ */
typedef struct {
    const char       *name;
    sandbox_opcode_t  opcode;
} opcode_entry_t;

static const opcode_entry_t opcode_table[] = {
    { "nop",        OP_NOP        },
    { "read",       OP_READ       },
    { "write",      OP_WRITE      },
    { "info",       OP_INFO       },
    { "echo",       OP_ECHO       },
    { "el",         OP_EL         },
    { "caps",       OP_CAPS       },
    { "if",         OP_IF         },
    { "sleep",      OP_SLEEP      },
    { "introspect", OP_INTROSPECT },
    { "wait_event", OP_WAIT_EVENT },
    { "respond",    OP_RESPOND    },
    { "model_call", OP_MODEL_CALL },
};

#define OPCODE_TABLE_SIZE (sizeof(opcode_table) / sizeof(opcode_table[0]))

/* ---- Internal string helpers ------------------------------------------- */

static void safe_strcpy(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (i < max - 1U && src[i] != '\0') { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool parse_uint64(const char *s, uint64_t *out)
{
    if (!s || s[0] == '\0') return false;
    uint64_t result = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (s[0] == '\0') return false;
        while (*s) {
            uint8_t n;
            if      (*s >= '0' && *s <= '9') n = (uint8_t)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') n = (uint8_t)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F') n = (uint8_t)(*s - 'A' + 10);
            else return false;
            result = (result << 4) | n;
            s++;
        }
    } else {
        while (*s) {
            if (*s < '0' || *s > '9') return false;
            result = result * 10U + (uint64_t)(*s - '0');
            s++;
        }
    }
    *out = result;
    return true;
}

/* ======================================================================
 * Variable store
 * ====================================================================== */

bool var_store_set(var_store_t *store, const char *name, uint64_t value)
{
    if (!store || !name || name[0] == '\0') return false;

    /* Update existing entry if name matches */
    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            store->entries[i].value = value;
            return true;
        }
    }

    /* Find an empty slot */
    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (!store->entries[i].set) {
            safe_strcpy(store->entries[i].name, name, SANDBOX_ARG_MAX_LEN);
            store->entries[i].value = value;
            store->entries[i].set   = true;
            return true;
        }
    }

    return false; /* Store full */
}

bool var_store_get(const var_store_t *store, const char *name, uint64_t *out)
{
    if (!store || !name || !out) return false;

    for (size_t i = 0; i < VAR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            *out = store->entries[i].value;
            return true;
        }
    }
    return false;
}

/* ---- Resolve an operand to a uint64 value ------------------------------ */
static bool resolve_operand(const operand_t *op,
                             const var_store_t *store,
                             uint64_t *out)
{
    if (op->kind == OPERAND_LITERAL) {
        *out = op->literal;
        return true;
    }
    return var_store_get(store, op->var_name, out);
}

/* ---- Evaluate a condition ---------------------------------------------- */
static bool eval_cond(const pipeline_cond_t *cond,
                      const var_store_t *store,
                      bool *result)
{
    uint64_t lval, rval;
    if (!resolve_operand(&cond->left,  store, &lval)) return false;
    if (!resolve_operand(&cond->right, store, &rval)) return false;

    switch (cond->op) {
        case CMP_EQ:  *result = (lval == rval); break;
        case CMP_NEQ: *result = (lval != rval); break;
        case CMP_LT:  *result = (lval <  rval); break;
        case CMP_GT:  *result = (lval >  rval); break;
        case CMP_LTE: *result = (lval <= rval); break;
        case CMP_GTE: *result = (lval >= rval); break;
        default: return false;
    }
    return true;
}

/* ======================================================================
 * str_store helpers
 * ====================================================================== */
bool str_store_set(str_store_t *store, const char *name, const char *value)
{
    if (!store || !name || !value) return false;
    /* Update existing */
    for (size_t i = 0; i < STR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            safe_strcpy(store->entries[i].value, value, MODEL_RESULT_MAX_LEN);
            return true;
        }
    }
    /* New entry */
    for (size_t i = 0; i < STR_STORE_SIZE; i++) {
        if (!store->entries[i].set) {
            safe_strcpy(store->entries[i].name,  name,  SANDBOX_ARG_MAX_LEN);
            safe_strcpy(store->entries[i].value, value, MODEL_RESULT_MAX_LEN);
            store->entries[i].set = true;
            return true;
        }
    }
    return false;
}

bool str_store_get(const str_store_t *store, const char *name,
                   char *out, size_t max)
{
    if (!store || !name || !out || max == 0) return false;
    for (size_t i = 0; i < STR_STORE_SIZE; i++) {
        if (store->entries[i].set &&
            k_strcmp(store->entries[i].name, name) == 0) {
            safe_strcpy(out, store->entries[i].value, max);
            return true;
        }
    }
    return false;
}

/* ======================================================================
 * interpreter_parse — single tool call into ast_node_t
 * ====================================================================== */
bool interpreter_parse(const char *input, ast_node_t *out_node)
{
    if (!input || !out_node) return false;
    k_memset(out_node, 0, sizeof(ast_node_t));
    out_node->opcode = OP_INVALID;

    /* Extract opcode token */
    char opcode_str[SANDBOX_ARG_MAX_LEN];
    size_t i = 0;
    while (input[i] != '\0' && input[i] != ' ' && i < SANDBOX_ARG_MAX_LEN - 1U) {
        opcode_str[i] = input[i]; i++;
    }
    opcode_str[i] = '\0';
    if (i == 0) return false;

    /* Look up opcode */
    bool found = false;
    for (size_t j = 0; j < OPCODE_TABLE_SIZE; j++) {
        if (k_strcmp(opcode_str, opcode_table[j].name) == 0) {
            out_node->opcode = opcode_table[j].opcode;
            found = true;
            break;
        }
    }
    if (!found) return false;

    /* Tokenise arguments */
    const char *cursor = input + i;
    uint32_t argc = 0;

    while (*cursor != '\0' && argc < SANDBOX_MAX_ARGS) {
        if (*cursor == ' ') { cursor++; continue; }

        /* Stop at "->" output binding marker */
        if (cursor[0] == '-' && cursor[1] == '>') break;

        size_t arg_len = 0;
        while (cursor[arg_len] != '\0' &&
               cursor[arg_len] != ' '  &&
               arg_len < SANDBOX_ARG_MAX_LEN - 1U) {
            arg_len++;
        }
        if (arg_len == 0) break;

        /* Copy exactly arg_len bytes — safe_strcpy copies until NUL which
         * would include trailing args; we must stop at the token boundary. */
        size_t copy_len = arg_len < SANDBOX_ARG_MAX_LEN - 1U
                        ? arg_len : SANDBOX_ARG_MAX_LEN - 1U;
        for (size_t k = 0; k < copy_len; k++)
            out_node->args[argc][k] = cursor[k];
        out_node->args[argc][copy_len] = '\0';
        cursor += arg_len;
        argc++;
    }

    out_node->argc = argc;
    return true;
}

/* ======================================================================
 * str_store helpers
 * ====================================================================== */

/* ======================================================================
 * interpreter_parse_pipeline — semicolon-separated steps into pipeline_t
 *
 * Format per step:
 *   <opcode> [arg0] [arg1] [-> varname]
 *
 * Steps are separated by ';'.
 * Output binding "-> varname" is parsed from the end of each step.
 *
 * OP_IF format:
 *   if <left> <op> <right> -> then:<N> [else:<M>]
 *   where N and M are the number of following steps in each branch.
 *   e.g.:  "if 1 == 1 -> then:2 else:1; echo yes; nop; echo no"
 * ====================================================================== */

/* Helper: parse a single step token (up to ';' or end) into pipeline_node_t */
static bool parse_pipeline_step(const char *input, size_t len,
                                 pipeline_node_t *out)
{
    if (!input || len == 0 || !out) return false;

    /* Copy step text into a local NUL-terminated buffer */
    char buf[128];
    if (len >= sizeof(buf)) return false;
    for (size_t i = 0; i < len; i++) buf[i] = input[i];
    buf[len] = '\0';

    k_memset(out, 0, sizeof(pipeline_node_t));
    out->opcode = OP_INVALID;

    /* ---- Check for "-> varname" output binding at end of step ---- */
    /* Scan backwards for "->" */
    int arrow_pos = -1;
    for (int i = (int)len - 1; i >= 1; i--) {
        if (buf[i-1] == '-' && buf[i] == '>') {
            arrow_pos = i - 1;
            break;
        }
    }

    if (arrow_pos >= 0) {
        /* Extract output var name (skip "-> " whitespace) */
        const char *var_start = buf + arrow_pos + 2;
        while (*var_start == ' ') var_start++;

        /* Handle OP_IF branch counts: "then:N" or "else:M" */
        if (k_strncmp(var_start, "then:", 5) == 0) {
            uint64_t then_count;
            if (!parse_uint64(var_start + 5, &then_count)) return false;
            out->then_count = (uint32_t)then_count;

            /* Check for else:M after then:N */
            const char *else_ptr = var_start + 5;
            while (*else_ptr >= '0' && *else_ptr <= '9') else_ptr++;
            while (*else_ptr == ' ') else_ptr++;
            if (k_strncmp(else_ptr, "else:", 5) == 0) {
                uint64_t else_count;
                if (!parse_uint64(else_ptr + 5, &else_count)) return false;
                out->else_count = (uint32_t)else_count;
            }
        } else {
            /* Regular output variable binding */
            safe_strcpy(out->output_var, var_start, SANDBOX_ARG_MAX_LEN);
        }

        /* Truncate buf at the arrow */
        buf[arrow_pos] = '\0';
        len = (size_t)arrow_pos;
    }

    /* ---- Trim trailing spaces ---- */
    while (len > 0 && buf[len - 1] == ' ') { buf[--len] = '\0'; }
    if (len == 0) return false;

    /* ---- Parse opcode ---- */
    char opcode_str[SANDBOX_ARG_MAX_LEN];
    size_t i = 0;
    while (buf[i] != '\0' && buf[i] != ' ' && i < SANDBOX_ARG_MAX_LEN - 1U) {
        opcode_str[i] = buf[i]; i++;
    }
    opcode_str[i] = '\0';
    if (i == 0) return false;

    bool found = false;
    for (size_t j = 0; j < OPCODE_TABLE_SIZE; j++) {
        if (k_strcmp(opcode_str, opcode_table[j].name) == 0) {
            out->opcode = opcode_table[j].opcode;
            found = true;
            break;
        }
    }
    if (!found) return false;

    const char *cursor = buf + i;
    uint32_t argc = 0;

    /* ---- OP_IF: parse condition operands ---- */
    if (out->opcode == OP_IF) {
        /*
         * Format: if <left> <op> <right>
         * left/right are either a number literal or a variable name.
         * op is one of: == != < > <= >=
         */
        while (*cursor == ' ') cursor++;

        /* Parse left operand */
        char left_str[SANDBOX_ARG_MAX_LEN];
        size_t k = 0;
        while (cursor[k] != ' ' && cursor[k] != '\0' && k < SANDBOX_ARG_MAX_LEN - 1U) {
            left_str[k] = cursor[k];
            k++;
        }
        left_str[k] = '\0';
        cursor += k;
        while (*cursor == ' ') cursor++;

        uint64_t lval;
        if (parse_uint64(left_str, &lval)) {
            out->cond.left.kind    = OPERAND_LITERAL;
            out->cond.left.literal = lval;
        } else {
            out->cond.left.kind = OPERAND_VARIABLE;
            safe_strcpy(out->cond.left.var_name, left_str, SANDBOX_ARG_MAX_LEN);
        }

        /* Parse operator */
        char op_str[4] = {0};
        size_t op_len = 0;
        while (cursor[op_len] != ' ' && cursor[op_len] != '\0' && op_len < 3U) {
            op_str[op_len] = cursor[op_len];
            op_len++;
        }
        op_str[op_len] = '\0';
        cursor += op_len;
        while (*cursor == ' ') cursor++;

        if      (k_strcmp(op_str, "==") == 0) out->cond.op = CMP_EQ;
        else if (k_strcmp(op_str, "!=") == 0) out->cond.op = CMP_NEQ;
        else if (k_strcmp(op_str, "<")  == 0) out->cond.op = CMP_LT;
        else if (k_strcmp(op_str, ">")  == 0) out->cond.op = CMP_GT;
        else if (k_strcmp(op_str, "<=") == 0) out->cond.op = CMP_LTE;
        else if (k_strcmp(op_str, ">=") == 0) out->cond.op = CMP_GTE;
        else return false;

        /* Parse right operand */
        char right_str[SANDBOX_ARG_MAX_LEN];
        k = 0;
        while (cursor[k] != ' ' && cursor[k] != '\0' && k < SANDBOX_ARG_MAX_LEN - 1U) {
            right_str[k] = cursor[k];
            k++;
        }
        right_str[k] = '\0';

        uint64_t rval;
        if (parse_uint64(right_str, &rval)) {
            out->cond.right.kind    = OPERAND_LITERAL;
            out->cond.right.literal = rval;
        } else {
            out->cond.right.kind = OPERAND_VARIABLE;
            safe_strcpy(out->cond.right.var_name, right_str, SANDBOX_ARG_MAX_LEN);
        }

        out->argc = 0;
        return true;
    }

    /* ---- Regular opcodes: tokenise arguments ---- */
    while (*cursor != '\0' && argc < SANDBOX_MAX_ARGS) {
        if (*cursor == ' ') { cursor++; continue; }
        size_t arg_len = 0;
        while (cursor[arg_len] != '\0' &&
               cursor[arg_len] != ' '  &&
               arg_len < SANDBOX_ARG_MAX_LEN - 1U) {
            arg_len++;
        }
        if (arg_len == 0) break;
        /* Copy exactly arg_len bytes to avoid including trailing tokens */
        size_t copy_len = arg_len < SANDBOX_ARG_MAX_LEN - 1U
                        ? arg_len : SANDBOX_ARG_MAX_LEN - 1U;
        for (size_t k = 0; k < copy_len; k++)
            out->args[argc][k] = cursor[k];
        out->args[argc][copy_len] = '\0';
        cursor += arg_len;
        argc++;
    }
    out->argc = argc;
    return true;
}

bool interpreter_parse_pipeline(const char *input, pipeline_t *out_pipeline)
{
    if (!input || !out_pipeline) return false;
    k_memset(out_pipeline, 0, sizeof(pipeline_t));

    const char *cursor = input;
    uint32_t    step   = 0;

    while (*cursor != '\0' && step < PIPELINE_MAX_STEPS) {
        /* Skip leading spaces */
        while (*cursor == ' ') cursor++;
        if (*cursor == '\0') break;

        /* Find end of this step (next ';' or end of string) */
        const char *start = cursor;
        size_t      len   = 0;
        while (cursor[len] != ';' && cursor[len] != '\0') len++;

        if (len == 0) { cursor++; continue; }

        if (!parse_pipeline_step(start, len, &out_pipeline->steps[step]))
            return false;

        step++;
        cursor = start + len;
        if (*cursor == ';') cursor++;
    }

    out_pipeline->step_count = step;
    return step > 0;
}

/* ======================================================================
 * interpreter_exec — execute one verified ast_node_t
 * ====================================================================== */
sandbox_result_t interpreter_exec(ast_node_t *node, sandbox_ctx_t *ctx)
{
    if (!node || !ctx) return SANDBOX_ERR_UNKNOWN;

    if (ctx->instruction_count >= SANDBOX_MAX_INSNS) return SANDBOX_ERR_LIMIT;
    ctx->instruction_count++;

    switch (node->opcode) {

        case OP_NOP:
            break;

        case OP_ECHO:
            sys_uart_write(node->args[0], k_strlen(node->args[0]), ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;

        case OP_READ: {
            uint64_t addr, len;
            parse_uint64(node->args[0], &addr);
            parse_uint64(node->args[1], &len);
            if (sys_mem_read((uintptr_t)addr, ctx->scratch,
                             (size_t)len, ctx->caps) != 0)
                return SANDBOX_ERR_FAULT;
            sys_uart_write("read: ", 6, ctx->caps);
            for (size_t i = 0; i < (size_t)len; i++) {
                uint8_t b  = ctx->scratch[i];
                char    hi = (b >> 4) < 10u ? (char)('0'+(b>>4))
                                             : (char)('A'+(b>>4)-10);
                char    lo = (b & 0xF) < 10u ? (char)('0'+(b&0xF))
                                              : (char)('A'+(b&0xF)-10);
                sys_uart_write(&hi, 1, ctx->caps);
                sys_uart_write(&lo, 1, ctx->caps);
                sys_uart_write(" ",  1, ctx->caps);
            }
            sys_uart_write("\n", 1, ctx->caps);
            ctx->scratch_used = (size_t)len;
            break;
        }

        case OP_WRITE: {
            uint64_t offset, value;
            parse_uint64(node->args[0], &offset);
            parse_uint64(node->args[1], &value);
            ctx->scratch[offset] = (uint8_t)value;
            if (offset >= ctx->scratch_used) ctx->scratch_used = offset + 1U;
            break;
        }

        case OP_INFO: {
            uintptr_t bs, be, ss, se;
            if (sys_mem_info(&bs, &be, &ss, &se, ctx->caps) != 0)
                return SANDBOX_ERR_DENIED;
            sys_uart_write("BSS   start : ", 14, ctx->caps); sys_uart_hex64(bs, ctx->caps);
            sys_uart_write("\nBSS   end   : ", 15, ctx->caps); sys_uart_hex64(be, ctx->caps);
            sys_uart_write("\nStack start : ", 15, ctx->caps); sys_uart_hex64(ss, ctx->caps);
            sys_uart_write("\nStack end   : ", 15, ctx->caps); sys_uart_hex64(se, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        case OP_EL: {
            uint64_t el;
            __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
            uint32_t el_val = (uint32_t)((el >> 2U) & 3U);
            sys_uart_write("EL: ", 4, ctx->caps);
            char c = (char)('0' + el_val);
            sys_uart_write(&c, 1, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;
        }

        case OP_CAPS:
            sys_uart_write("caps: ", 6, ctx->caps);
            sys_uart_hex64((uint64_t)ctx->caps, ctx->caps);
            sys_uart_write("\n", 1, ctx->caps);
            break;

        case OP_SLEEP: {
            /*
             * Busy-wait for the requested number of milliseconds.
             *
             * Uses the ARMv8 generic timer (CNTPCT_EL0) for a portable
             * delay that doesn't depend on loop iteration count or CPU
             * frequency assumptions.
             *
             * CNTFRQ_EL0 gives the timer frequency in Hz.
             * ticks_per_ms = freq / 1000.
             * We read the counter, add the required ticks, spin until
             * CNTPCT_EL0 >= deadline.
             */
            uint64_t ms;
            parse_uint64(node->args[0], &ms);

            uint64_t freq;
            __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (freq));
            uint64_t ticks_per_ms = freq / 1000ULL;

            uint64_t start;
            __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (start));
            uint64_t deadline = start + ms * ticks_per_ms;

            uint64_t now;
            do {
                __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (now));
            } while (now < deadline);
            break;
        }

        case OP_INTROSPECT: {
            /*
             * Print the whitelisted MMIO address map so an LLM or
             * operator can inspect what the sandbox is allowed to access.
             *
             * Format (one entry per line):
             *   MMIO <friendly_name> : <hex_address>
             *
             * This gives an AI agent a self-description of its environment
             * before it starts writing pipelines — "inspect before plan."
             */
            static const struct {
                const char *name;
                uintptr_t   addr;
            } mmio_map[] = {
                { "UART0_DR",     0x09000000UL },
                { "UART0_FR",     0x09000018UL },
                { "GICD_CTLR",    0x08000000UL },
                { "GICC_IAR",     0x0801000CUL },
                { "CNTPCT_EL0",   0x00000000UL }, /* system reg, not MMIO */
            };
            static const size_t mmio_map_count =
                sizeof(mmio_map) / sizeof(mmio_map[0]);

            sys_uart_write("MMIO map:\n", 10, ctx->caps);
            for (size_t m = 0; m < mmio_map_count; m++) {
                sys_uart_write("  ", 2, ctx->caps);
                sys_uart_write(mmio_map[m].name,
                               k_strlen(mmio_map[m].name), ctx->caps);
                sys_uart_write(" : ", 3, ctx->caps);
                sys_uart_hex64((uint64_t)mmio_map[m].addr, ctx->caps);
                sys_uart_write("\n", 1, ctx->caps);
            }
            break;
        }

        case OP_WAIT_EVENT:
            /*
             * Yield stub — issues WFE (Wait For Event) once.
             *
             * In a full implementation this would suspend the pipeline
             * until an event register is set by an IRQ handler, enabling
             * asynchronous pipeline coordination without busy-waiting.
             * For now it provides a single low-power yield point.
             */
            __asm__ volatile ("wfe");
            break;

        case OP_RESPOND: {
            /*
             * Emit a structured RESPOND:{...} JSON packet on UART.
             *
             * Format:
             *   RESPOND:{"status":"ok","goal":"<arg0>","vars":{<var_store>},"caps":"0x...","el":<N>}
             *
             * The agent bridge detects lines beginning with "RESPOND:" and
             * parses the JSON rather than treating it as free-form output.
             * This gives the LLM typed values instead of text to interpret.
             *
             * Usage in AIQL:
             *   respond system_state         — goal label from arg0
             *   respond                      — anonymous respond
             */

            /* --- helpers: write key:"hex" and key:N ------------------- */
            #define UART_STR(s)  sys_uart_write((s), k_strlen(s), ctx->caps)
            #define UART_CHR(c)  do { char _c=(c); sys_uart_write(&_c,1,ctx->caps); } while(0)

            UART_STR("RESPOND:{");

            /* status */
            UART_STR("\"status\":\"ok\"");

            /* goal — from arg0 if provided */
            if (node->argc > 0 && node->args[0][0] != '\0') {
                UART_STR(",\"goal\":\"");
                UART_STR(node->args[0]);
                UART_CHR('"');
            }

            /* caps */
            UART_STR(",\"caps\":\"0x");
            {
                uint64_t v = (uint64_t)ctx->caps;
                char hex[17]; int hi = 16; hex[hi] = '\0';
                do {
                    uint8_t n = (uint8_t)(v & 0xFU);
                    hex[--hi] = n < 10u ? (char)('0'+n) : (char)('a'+n-10);
                    v >>= 4;
                } while (v && hi > 0);
                UART_STR(hex + hi);
            }
            UART_CHR('"');

            /* el */
            {
                uint64_t el;
                __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
                uint32_t el_val = (uint32_t)((el >> 2U) & 3U);
                UART_STR(",\"el\":"); UART_CHR((char)('0' + el_val));
            }

            /* var store — emit every set variable as key:value.
             * If a string value exists in str_vars, emit it as "s":"...".
             * Always emit the numeric hash as "v":"0x..." for completeness. */
            bool first_var = true;
            UART_STR(",\"vars\":{");
            for (size_t vi = 0; vi < VAR_STORE_SIZE; vi++) {
                var_entry_t *ve = &ctx->vars.entries[vi];
                if (!ve->set || ve->name[0] == '\0') continue;
                if (!first_var) UART_CHR(',');
                first_var = false;
                UART_CHR('"');
                UART_STR(ve->name);
                UART_STR("\":{\"v\":\"0x");
                /* hex encode numeric value */
                uint64_t v = ve->value;
                char hex[17]; int hi = 16; hex[hi] = '\0';
                do {
                    uint8_t n = (uint8_t)(v & 0xFU);
                    hex[--hi] = n < 10u ? (char)('0'+n) : (char)('a'+n-10);
                    v >>= 4;
                } while (v && hi > 0);
                UART_STR(hex + hi);
                UART_CHR('"');
                /* also emit string value if present in str_vars — JSON-escaped */
                char sbuf[MODEL_RESULT_MAX_LEN];
                if (str_store_get(&ctx->str_vars, ve->name, sbuf, sizeof(sbuf))) {
                    UART_STR(",\"s\":\"");
                    for (const char *sp = sbuf; *sp; sp++) {
                        if (*sp == '"' || *sp == '\\') UART_CHR('\\');
                        UART_CHR(*sp);
                    }
                    UART_CHR('"');
                }
                UART_STR("}");
            }
            UART_CHR('}');

            UART_STR("}\r\n");

            #undef UART_STR
            #undef UART_CHR
            break;
        }

        case OP_MODEL_CALL: {
            /*
             * EXEC: protocol — synchronous host LLM/classifier call.
             *
             * 1. Emit EXEC:{...} JSON line on UART
             * 2. Block reading UART lines until "RESULT:{...}" arrives
             * 3. Parse result value, store in var_store (hash) and str_store
             *
             * The host bridge (kai_agent.py) intercepts EXEC: before the
             * prompt returns, makes the actual API call, and writes RESULT:.
             *
             * EXEC packet format:
             *   EXEC:{"type":"llm","action":"<action>","input":"<input>",
             *          "output_var":"<var>","seq":<n>}
             *
             * RESULT packet format (host writes this):
             *   RESULT:{"value":"<text>","seq":<n>}
             */

            #define USTR(s)  sys_uart_write((s), k_strlen(s), ctx->caps)
            #define UCHR(c)  do { char _c=(c); sys_uart_write(&_c,1,ctx->caps); } while(0)

            /* Build and emit EXEC packet */
            USTR("EXEC:{");
            USTR("\"type\":\"");
            USTR("llm");  /* model_type not available in single-shot exec */
            USTR("\",\"action\":\"");
            USTR("call");  /* model_action not available in single-shot exec */
            USTR("\",\"input\":\"");
            /* Emit input, escaping quotes and backslashes */
            for (const char *ip = ""; *ip; ip++) {  /* no input in ast_node_t */
                if (*ip == '"'  || *ip == '\\') UCHR('\\');
                UCHR(*ip);
            }
            USTR("\"");
            /* output_var not on ast_node_t — omit from single-shot exec */
            USTR("}\r\n");

            /* Block-read UART lines until RESULT: arrives (or 512 chars timeout) */
            char result_buf[MODEL_RESULT_MAX_LEN];
            k_memset(result_buf, 0, sizeof(result_buf));
            bool got_result = false;

            /* Read up to 4 lines looking for RESULT: */
            for (int attempt = 0; attempt < 4 && !got_result; attempt++) {
                char line[MODEL_RESULT_MAX_LEN];
                size_t li = 0;
                /* Read one line (CR or LF terminated) */
                while (li < sizeof(line) - 1U) {
                    char rc = uart_getc();
                    if (rc == '\r' || rc == '\n') {
                        if (li > 0) break;
                        continue;
                    }
                    line[li++] = rc;
                }
                line[li] = '\0';

                /* Check for RESULT: prefix */
                if (k_strncmp(line, "RESULT:", 7) == 0) {
                    /* Extract "value" field from JSON: RESULT:{"value":"..."} */
                    /* value key scanned inline below */
                    size_t vklen = 8; /* len of value":" */
                    /* Simple scan for "value":"  */
                    for (size_t si = 7; si + vklen < li; si++) {
                        if (k_strncmp(line + si, "\"value\":\"", 10) == 0) {
                            const char *vs = line + si + 10;
                            size_t vi2 = 0;
                            while (*vs && *vs != '"' && vi2 < sizeof(result_buf)-1)
                                result_buf[vi2++] = *vs++;
                            result_buf[vi2] = '\0';
                            got_result = true;
                            break;
                        }
                    }
                    if (!got_result) {
                        /* Fallback: take everything after RESULT: as raw value */
                        safe_strcpy(result_buf, line + 7, sizeof(result_buf));
                        got_result = true;
                    }
                }
            }

            if (!got_result) {
                /* Timeout — store empty string, pipeline continues */
                safe_strcpy(result_buf, "timeout", sizeof(result_buf));
            }

            /* Store string result */
            /* output_var storage skipped in single-shot interpreter_exec */
            (void)result_buf;

            /* Echo result to UART for visibility */
            USTR("[model] result: ");
            USTR(result_buf);
            USTR("\r\n");

            #undef USTR
            #undef UCHR
            break;
        }

        default:
            return SANDBOX_ERR_UNKNOWN;
    }

    return SANDBOX_OK;
}

/* ======================================================================
 * interpreter_exec_pipeline — execute a verified pipeline_t
 *
 * Steps execute in order. OP_IF evaluates its condition against the
 * variable store and skips then_count or else_count steps accordingly.
 * Output bindings store the insn_count at time of execution as a
 * simple numeric result that later steps can reference.
 * ====================================================================== */
sandbox_result_t interpreter_exec_pipeline(pipeline_t *pipeline,
                                            sandbox_ctx_t *ctx)
{
    if (!pipeline || !ctx) return SANDBOX_ERR_UNKNOWN;

    uint32_t i = 0;

    while (i < pipeline->step_count) {
        if (ctx->instruction_count >= SANDBOX_MAX_INSNS) return SANDBOX_ERR_LIMIT;

        pipeline_node_t *step = &pipeline->steps[i];

        /* ---- OP_IF: evaluate condition, skip branches ---------------- */
        if (step->opcode == OP_IF) {
            ctx->instruction_count++;
            bool cond_result = false;
            if (!eval_cond(&step->cond, &ctx->vars, &cond_result))
                return SANDBOX_ERR_COND;

            if (cond_result) {
                /* Execute then-branch steps — they follow immediately */
                i++;
                /* Steps i .. i+then_count-1 will execute normally in
                 * subsequent loop iterations; skip else-branch after */
                /* Mark else steps to skip by jumping past them after then */
                /* We handle this by letting the loop run then-branch
                 * naturally, then inserting a skip after */
                uint32_t then_end  = i + step->then_count;
                uint32_t else_skip = step->else_count;

                /* Execute then steps */
                while (i < then_end && i < pipeline->step_count) {
                    ast_node_t tmp;
                    k_memset(&tmp, 0, sizeof(ast_node_t));
                    tmp.opcode = pipeline->steps[i].opcode;
                    tmp.argc   = pipeline->steps[i].argc;
                    for (uint32_t a = 0; a < pipeline->steps[i].argc; a++)
                        safe_strcpy(tmp.args[a], pipeline->steps[i].args[a],
                                    SANDBOX_ARG_MAX_LEN);

                    sandbox_result_t r = interpreter_exec(&tmp, ctx);
                    if (r != SANDBOX_OK) return r;

                    /* Output binding */
                    if (pipeline->steps[i].output_var[0] != '\0') {
                        if (!var_store_set(&ctx->vars,
                                           pipeline->steps[i].output_var,
                                           (uint64_t)ctx->instruction_count))
                            return SANDBOX_ERR_VAR;
                    }
                    i++;
                }
                /* Skip else-branch */
                i += else_skip;
            } else {
                /* Skip then-branch, execute else-branch if present */
                i += step->then_count + 1;

                uint32_t else_end = i + step->else_count;
                while (i < else_end && i < pipeline->step_count) {
                    ast_node_t tmp;
                    k_memset(&tmp, 0, sizeof(ast_node_t));
                    tmp.opcode = pipeline->steps[i].opcode;
                    tmp.argc   = pipeline->steps[i].argc;
                    for (uint32_t a = 0; a < pipeline->steps[i].argc; a++)
                        safe_strcpy(tmp.args[a], pipeline->steps[i].args[a],
                                    SANDBOX_ARG_MAX_LEN);

                    sandbox_result_t r = interpreter_exec(&tmp, ctx);
                    if (r != SANDBOX_OK) return r;

                    if (pipeline->steps[i].output_var[0] != '\0') {
                        if (!var_store_set(&ctx->vars,
                                           pipeline->steps[i].output_var,
                                           (uint64_t)ctx->instruction_count))
                            return SANDBOX_ERR_VAR;
                    }
                    i++;
                }
            }
            continue;
        }

        /* ---- OP_MODEL_CALL: dispatch directly with pipeline_node_t fields */
        if (step->opcode == OP_MODEL_CALL) {
            ctx->instruction_count++;
            #define PSTR(s)  sys_uart_write((s), k_strlen(s), ctx->caps)
            #define PCHR(c)  do { char _pc=(c); sys_uart_write(&_pc,1,ctx->caps); } while(0)

            PSTR("EXEC:{");
            PSTR("\"type\":\"");
            PSTR(step->model_type[0] ? step->model_type : "llm");
            PSTR("\",\"action\":\"");
            PSTR(step->model_action[0] ? step->model_action : "call");
            PSTR("\",\"input\":\"");
            for (const char *ip = step->model_input; *ip; ip++) {
                if (*ip == '"' || *ip == '\\') PCHR('\\');
                PCHR(*ip);
            }
            PSTR("\"");
            if (step->output_var[0]) {
                PSTR(",\"output_var\":\"");
                PSTR(step->output_var);
                PCHR('"');
            }
            PSTR("}\r\n");

            /* Block-read until RESULT: */
            char pbuf[MODEL_RESULT_MAX_LEN];
            k_memset(pbuf, 0, sizeof(pbuf));
            bool pgot = false;
            for (int att = 0; att < 8 && !pgot; att++) {
                char pline[MODEL_RESULT_MAX_LEN];
                size_t pli = 0;
                while (pli < sizeof(pline) - 1U) {
                    char rc = uart_getc();
                    if (rc == '\r' || rc == '\n') { if (pli > 0) break; continue; }
                    pline[pli++] = rc;
                }
                pline[pli] = '\0';
                if (k_strncmp(pline, "RESULT:", 7) == 0) {
                    for (size_t si = 7; si + 9 < pli; si++) {
                        if (k_strncmp(pline + si, "\"value\":\"", 9) == 0) {
                            const char *vs = pline + si + 9;
                            size_t vi2 = 0;
                            while (*vs && *vs != '"' && vi2 < sizeof(pbuf)-1)
                                pbuf[vi2++] = *vs++;
                            pbuf[vi2] = '\0';
                            pgot = true; break;
                        }
                    }
                    if (!pgot) { safe_strcpy(pbuf, pline + 7, sizeof(pbuf)); pgot = true; }
                }
            }
            if (!pgot) safe_strcpy(pbuf, "timeout", sizeof(pbuf));

            if (step->output_var[0]) {
                str_store_set(&ctx->str_vars, step->output_var, pbuf);
                uint64_t hash = 5381ULL;
                for (const char *hp = pbuf; *hp; hp++)
                    hash = ((hash << 5) + hash) ^ (uint64_t)(unsigned char)*hp;
                var_store_set(&ctx->vars, step->output_var, hash);
            }
            PSTR("[model] "); PSTR(pbuf); PSTR("\r\n");
            #undef PSTR
            #undef PCHR
            i++;
            continue;
        }

        /* ---- Regular step ------------------------------------------- */
        ast_node_t tmp;
        k_memset(&tmp, 0, sizeof(ast_node_t));
        tmp.opcode = step->opcode;
        tmp.argc   = step->argc;
        for (uint32_t a = 0; a < step->argc; a++)
            safe_strcpy(tmp.args[a], step->args[a], SANDBOX_ARG_MAX_LEN);

        sandbox_result_t r = interpreter_exec(&tmp, ctx);
        if (r != SANDBOX_OK) return r;

        /* Output binding */
        if (step->output_var[0] != '\0') {
            if (!var_store_set(&ctx->vars, step->output_var,
                               (uint64_t)ctx->instruction_count))
                return SANDBOX_ERR_VAR;
        }

        i++;
    }

    return SANDBOX_OK;
}