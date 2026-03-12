/*
 * src/aiql.c — Kernel-side AIQL JSON extractor
 *
 * Targeted schema walker: finds known keys in a fixed shape, builds
 * pipeline_t structs. Not a general JSON parser.
 *
 * Opcode mapping mirrors tools/aiql_to_kai.py exactly so host-compiled
 * and kernel-compiled programs behave identically.
 */

#include <kernel/aiql.h>
#include <kernel/sandbox.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Internal string helpers (no libc)
 * ========================================================================= */

static const char *j_strstr(const char *hay, size_t hlen,
                             const char *needle, size_t nlen)
{
    if (nlen == 0 || nlen > hlen) return (void *)0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool ok = true;
        for (size_t j = 0; j < nlen; j++)
            if (hay[i+j] != needle[j]) { ok = false; break; }
        if (ok) return hay + i;
    }
    return (void *)0;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n')) p++;
    return p;
}

/* Copy a JSON string value (stops at '"', max-1 chars) */
static void copy_str(char *dst, const char *src, size_t max, const char *end)
{
    size_t i = 0;
    while (src < end && *src && *src != '"' && i < max-1)
        dst[i++] = *src++;
    dst[i] = '\0';
}

/* Parse decimal or "0x..." integer, returning 0 on failure */
static uint64_t parse_num(const char *s, const char *end)
{
    while (s < end && (*s==' '||*s=='"')) s++;
    if (s >= end) return 0;
    uint64_t v = 0;
    if (s[0]=='0' && s+1<end && (s[1]=='x'||s[1]=='X')) {
        s += 2;
        while (s < end) {
            uint8_t n;
            if      (*s>='0'&&*s<='9') n=(uint8_t)(*s-'0');
            else if (*s>='a'&&*s<='f') n=(uint8_t)(*s-'a'+10);
            else if (*s>='A'&&*s<='F') n=(uint8_t)(*s-'A'+10);
            else break;
            v=(v<<4)|n; s++;
        }
    } else {
        while (s<end && *s>='0' && *s<='9') { v=v*10+(uint64_t)(*s-'0'); s++; }
    }
    return v;
}

/* uint64 → decimal string, writes into buf[16], returns buf */
static const char *u64_str(uint64_t v, char *buf)
{
    if (v == 0) { buf[0]='0'; buf[1]='\0'; return buf; }
    char tmp[20]; int i=19; tmp[i]='\0';
    while (v && i>0) { tmp[--i]=(char)('0'+(int)(v%10)); v/=10; }
    const char *s = tmp+i;
    int j=0;
    while (*s) buf[j++]=*s++;
    buf[j]='\0';
    return buf;
}

/* =========================================================================
 * JSON structural navigation
 * ========================================================================= */

/*
 * find_key — find the value start (char after ':') for a quoted key.
 * Searches within [region, region+rlen). Returns NULL if not found.
 */
static const char *find_key(const char *region, size_t rlen, const char *key)
{
    /* build '"key"' pattern on stack */
    char pat[64]; size_t klen = k_strlen(key);
    if (klen > 61) return (void *)0;
    pat[0]='"';
    for (size_t i=0;i<klen;i++) pat[i+1]=key[i];
    pat[klen+1]='"'; pat[klen+2]='\0';
    size_t plen = klen+2;

    const char *p=region, *lim=region+rlen;
    while ((size_t)(lim-p) > plen) {
        const char *hit = j_strstr(p, (size_t)(lim-p), pat, plen);
        if (!hit) return (void *)0;
        const char *after = hit+plen;
        if (after >= lim) return (void *)0;
        after = skip_ws(after, lim);
        if (after < lim && *after==':') return after+1;
        p = hit+1;
    }
    return (void *)0;
}

/* find_key then skip whitespace to the opening '"' and copy string value */
static void get_str(const char *region, size_t rlen, const char *key,
                    char *dst, size_t max)
{
    dst[0]='\0';
    const char *v = find_key(region, rlen, key);
    if (!v) return;
    const char *end = region+rlen;
    v = skip_ws(v, end);
    if (v<end && *v=='"') { v++; copy_str(dst, v, max, end); }
}

/* find_key then parse numeric value */
static uint64_t get_num(const char *region, size_t rlen, const char *key)
{
    const char *v = find_key(region, rlen, key);
    if (!v) return 0;
    return parse_num(v, region+rlen);
}

/* Find opening '{' for a named object key */
static const char *find_obj(const char *region, size_t rlen, const char *key)
{
    const char *v = find_key(region, rlen, key);
    if (!v) return (void *)0;
    v = skip_ws(v, region+rlen);
    return (v && *v=='{') ? v : (void *)0;
}

/* Find opening '[' for a named array key */
static const char *find_arr(const char *region, size_t rlen, const char *key)
{
    const char *v = find_key(region, rlen, key);
    if (!v) return (void *)0;
    v = skip_ws(v, region+rlen);
    return (v && *v=='[') ? v : (void *)0;
}

/* Matching '}' for an opening '{', respecting nesting + strings */
static const char *obj_end(const char *p, const char *end)
{
    if (!p || p>=end || *p!='{') return (void *)0;
    int depth=0; bool in_str=false;
    while (p<end) {
        if (*p=='"') in_str=!in_str;
        if (!in_str) {
            if (*p=='{') depth++;
            else if (*p=='}') { if(--depth==0) return p; }
        }
        p++;
    }
    return (void *)0;
}

/* Next '{' inside an array, stopping at ']' */
static const char *arr_next_obj(const char *p, const char *arr_end)
{
    while (p<arr_end) {
        if (*p=='{') return p;
        if (*p==']') return (void *)0;
        p++;
    }
    return (void *)0;
}

/* =========================================================================
 * Opcode name heuristic (mirrors aiql_to_kai.py)
 * ========================================================================= */

typedef struct { const char *frag; sandbox_opcode_t op; } op_entry_t;
static const op_entry_t OP_TABLE[] = {
    { "introspect",  OP_INTROSPECT },
    { "mmio",        OP_INTROSPECT },
    { "hardware_map",OP_INTROSPECT },
    { "meminfo",     OP_INFO       },
    { "memory_info", OP_INFO       },
    { "getmemory",   OP_INFO       },
    { "info",        OP_INFO       },
    { "exception",   OP_EL         },
    { "privilege",   OP_EL         },
    { "getexcept",   OP_EL         },
    { "caps",        OP_CAPS       },
    { "capabilit",   OP_CAPS       },
    { "permission",  OP_CAPS       },
    { "getcaps",     OP_CAPS       },
    { "read",        OP_READ       },
    { "sensor",      OP_READ       },
    { "fetch",       OP_READ       },
    { "write",       OP_WRITE      },
    { "set",         OP_WRITE      },
    { "actuate",     OP_WRITE      },
    { "motor",       OP_WRITE      },
    { "drive",       OP_WRITE      },
    { "sleep",       OP_SLEEP      },
    { "delay",       OP_SLEEP      },
    { "pause",       OP_SLEEP      },
    { "wait",        OP_SLEEP      },
    { "el",          OP_EL         },
    { "nop",         OP_NOP        },
    { "respond",     OP_RESPOND    },
    { "report",      OP_RESPOND    },
    { "result",      OP_RESPOND    },
    { "model_call",  OP_MODEL_CALL },
    { "model",       OP_MODEL_CALL },
    { "classify",    OP_MODEL_CALL },
    { "infer",       OP_MODEL_CALL },
};
#define OP_TABLE_LEN (sizeof(OP_TABLE)/sizeof(OP_TABLE[0]))

static char to_lc(char c) { return (c>='A'&&c<='Z')?(char)(c+32):c; }

static bool name_has(const char *name, size_t nlen,
                     const char *frag, size_t flen)
{
    if (flen > nlen) return false;
    for (size_t i=0; i<=nlen-flen; i++) {
        bool ok=true;
        for (size_t j=0;j<flen;j++)
            if (to_lc(name[i+j])!=to_lc(frag[j])) { ok=false; break; }
        if (ok) return true;
    }
    return false;
}

static sandbox_opcode_t name_to_op(const char *name)
{
    size_t nlen = k_strlen(name);
    for (size_t i=0;i<OP_TABLE_LEN;i++) {
        size_t flen = k_strlen(OP_TABLE[i].frag);
        if (name_has(name, nlen, OP_TABLE[i].frag, flen))
            return OP_TABLE[i].op;
    }
    return OP_ECHO; /* default: echo stub */
}

/* =========================================================================
 * Step builder
 * ========================================================================= */

static bool build_step(const char *obj, const char *oend,
                       pipeline_node_t *step)
{
    k_memset(step, 0, sizeof(pipeline_node_t));
    size_t osize = (size_t)(oend - obj);

    char type[32]={0};
    get_str(obj, osize, "type", type, sizeof(type));

    /* ---- ReturnStatement ---- */
    if (k_strncmp(type,"ReturnStatement",15)==0) {
        char var[SANDBOX_ARG_MAX_LEN]={0};
        get_str(obj, osize, "variable", var, sizeof(var));
        step->opcode = OP_ECHO; step->argc = 1;
        char *d = step->args[0];
        const char *pfx = "RETURN:"; size_t pi=0;
        while (pfx[pi] && pi<SANDBOX_ARG_MAX_LEN-1) { d[pi]=pfx[pi]; pi++; }
        size_t vi=0;
        while (var[vi] && pi<SANDBOX_ARG_MAX_LEN-1) d[pi++]=var[vi++];
        d[pi]='\0';
        return true;
    }

    /* ---- ConditionalStatement ---- */
    if (k_strncmp(type,"ConditionalStatement",20)==0) {
        step->opcode = OP_IF;
        const char *cobj = find_obj(obj, osize, "condition");
        if (!cobj) return false;
        const char *cend = obj_end(cobj, obj+osize);
        if (!cend) return false;
        size_t csz = (size_t)(cend-cobj);

        /* operator */
        char opstr[8]={0};
        get_str(cobj, csz, "operator", opstr, sizeof(opstr));
        cmp_op_t cmp=CMP_EQ;
        if (k_strcmp(opstr,"!=")==0) cmp=CMP_NEQ;
        else if (k_strcmp(opstr,"<")==0)  cmp=CMP_LT;
        else if (k_strcmp(opstr,">")==0)  cmp=CMP_GT;
        else if (k_strcmp(opstr,"<=")==0) cmp=CMP_LTE;
        else if (k_strcmp(opstr,">=")==0) cmp=CMP_GTE;
        step->cond.op = cmp;

        /* left */
        const char *lobj = find_obj(cobj, csz, "left");
        if (lobj) {
            const char *le = obj_end(lobj, cend);
            if (le) {
                char lt[16]={0}; size_t lsz=(size_t)(le-lobj);
                get_str(lobj, lsz, "type", lt, sizeof(lt));
                if (k_strncmp(lt,"Literal",7)==0) {
                    step->cond.left.kind    = OPERAND_LITERAL;
                    step->cond.left.literal = get_num(lobj, lsz, "value");
                } else {
                    step->cond.left.kind = OPERAND_VARIABLE;
                    get_str(lobj, lsz, "name",
                            step->cond.left.var_name, SANDBOX_ARG_MAX_LEN);
                }
            }
        }
        /* right */
        const char *robj = find_obj(cobj, csz, "right");
        if (robj) {
            const char *re = obj_end(robj, cend);
            if (re) {
                char rt[16]={0}; size_t rsz=(size_t)(re-robj);
                get_str(robj, rsz, "type", rt, sizeof(rt));
                if (k_strncmp(rt,"Literal",7)==0) {
                    step->cond.right.kind    = OPERAND_LITERAL;
                    step->cond.right.literal = get_num(robj, rsz, "value");
                } else {
                    step->cond.right.kind = OPERAND_VARIABLE;
                    get_str(robj, rsz, "name",
                            step->cond.right.var_name, SANDBOX_ARG_MAX_LEN);
                }
            }
        }

        /* branch step counts — count objects in then_body/else_body */
        uint32_t then_n=0, else_n=0;
        const char *tarr = find_arr(obj, osize, "then_body");
        if (tarr) {
            const char *tp=tarr+1;
            while ((tp=arr_next_obj(tp,obj+osize))!=((void *)0)) {
                const char *te=obj_end(tp,obj+osize);
                if (!te) break;
                then_n++; tp=te+1;
            }
        }
        const char *earr = find_arr(obj, osize, "else_body");
        if (earr) {
            const char *ep=earr+1;
            while ((ep=arr_next_obj(ep,obj+osize))!=((void *)0)) {
                const char *ee=obj_end(ep,obj+osize);
                if (!ee) break;
                else_n++; ep=ee+1;
            }
        }
        step->then_count = then_n ? then_n : 1;
        step->else_count = else_n;
        return true;
    }

    /* ---- Operation or CallStatement ---- */
    char name[SANDBOX_ARG_MAX_LEN]={0};
    /* Operation.name or CallStatement.action */
    get_str(obj, osize, "name", name, sizeof(name));
    if (!name[0]) get_str(obj, osize, "action", name, sizeof(name));

    /* call_type: llm/classifier/model → echo stub */
    char ct[16]={0};
    get_str(obj, osize, "call_type", ct, sizeof(ct));
    bool is_model = (k_strcmp(ct,"llm")==0 ||
                     k_strcmp(ct,"classifier")==0 ||
                     k_strcmp(ct,"model")==0);

    /* output variable */
    get_str(obj, osize, "output", step->output_var, SANDBOX_ARG_MAX_LEN);
    if (!step->output_var[0]) {
        const char *oa = find_arr(obj, osize, "outputs");
        if (oa) {
            const char *p=oa+1;
            while (p<obj+osize && *p!='"' && *p!=']') p++;
            if (p<obj+osize && *p=='"') { p++; copy_str(step->output_var, p, SANDBOX_ARG_MAX_LEN, obj+osize); }
        }
    }

    if (is_model) {
        /* Real OP_MODEL_CALL — populate model fields from AIQL CallStatement */
        step->opcode = OP_MODEL_CALL;
        step->argc   = 0;

        copy_str(step->model_type,   ct[0]   ? ct   : "llm",
                 sizeof(step->model_type),   ct   + sizeof(step->model_type));
        copy_str(step->model_action, name[0] ? name : "call",
                 sizeof(step->model_action), name + sizeof(step->model_action));

        /* Extract params inline — pobj not yet declared at this point */
        {
            const char *mp = find_obj(obj, osize, "params");
            const char *me = mp ? obj_end(mp, obj + osize) : (void *)0;
            size_t      msz = (mp && me) ? (size_t)(me - mp) : 0U;
            if (msz > 0) {
                char inp[MODEL_INPUT_MAX_LEN] = {0};
                get_str(mp, msz, "input",  inp, sizeof(inp));
                if (!inp[0]) get_str(mp, msz, "prompt", inp, sizeof(inp));
                if (!inp[0]) get_str(mp, msz, "text",   inp, sizeof(inp));
                if (!inp[0]) get_str(mp, msz, "query",  inp, sizeof(inp));
                /* Warn if input was silently truncated at MODEL_INPUT_MAX_LEN */
                if (inp[MODEL_INPUT_MAX_LEN-1] != '\0') {
                    uart_puts("[aiql] warn: model input truncated at 63 chars\r\n");
                }
                copy_str(step->model_input, inp, MODEL_INPUT_MAX_LEN,
                         inp + MODEL_INPUT_MAX_LEN);
            }
        }
        return true;
    }

    sandbox_opcode_t op = name_to_op(name);
    step->opcode = op;

    /* params object */
    const char *pobj = find_obj(obj, osize, "params");
    const char *pend = pobj ? obj_end(pobj, obj+osize) : (void *)0;
    size_t psz = (pobj && pend) ? (size_t)(pend-pobj) : 0;

    char num_buf[20];

    switch (op) {
        case OP_READ: {
            char addr[32]={0}, len[16]={0};
            if (psz) {
                get_str(pobj, psz, "address", addr, sizeof(addr));
                if (!addr[0]) get_str(pobj, psz, "addr", addr, sizeof(addr));
                uint64_t l = get_num(pobj, psz, "length");
                if (!l) l = get_num(pobj, psz, "len");
                if (!l) l = 4;
                u64_str(l, len);
            }
            if (!addr[0]) {
                /* no address → echo stub */
                step->opcode=OP_ECHO; step->argc=1;
                copy_str(step->args[0], name[0]?name:"read-stub",
                         SANDBOX_ARG_MAX_LEN, name+SANDBOX_ARG_MAX_LEN);
            } else {
                step->argc=2;
                copy_str(step->args[0], addr, SANDBOX_ARG_MAX_LEN, addr+sizeof(addr));
                copy_str(step->args[1], len,  SANDBOX_ARG_MAX_LEN, len+sizeof(len));
            }
            break;
        }
        case OP_WRITE: {
            uint64_t off = psz ? get_num(pobj, psz, "offset") : 0;
            char val[16]={0};
            if (psz) {
                get_str(pobj, psz, "value", val, sizeof(val));
                if (!val[0]) get_str(pobj, psz, "val", val, sizeof(val));
                if (!val[0]) { val[0]='0'; val[1]='\0'; }
            } else { val[0]='0'; val[1]='\0'; }
            step->argc=2;
            u64_str(off, num_buf);
            copy_str(step->args[0], num_buf, SANDBOX_ARG_MAX_LEN, num_buf+20);
            copy_str(step->args[1], val,     SANDBOX_ARG_MAX_LEN, val+sizeof(val));
            break;
        }
        case OP_SLEEP: {
            uint64_t ms = psz ? get_num(pobj, psz, "ms") : 0;
            if (!ms && psz) ms = get_num(pobj, psz, "duration");
            if (!ms && psz) ms = get_num(pobj, psz, "milliseconds");
            if (!ms) ms = 100;
            if (ms > 10000) ms = 10000;
            step->argc=1;
            u64_str(ms, num_buf);
            copy_str(step->args[0], num_buf, SANDBOX_ARG_MAX_LEN, num_buf+20);
            break;
        }
        case OP_RESPOND: {
            /* Optional goal label from params.goal or params.label */
            step->argc = 0;
            if (psz) {
                char goal[SANDBOX_ARG_MAX_LEN] = {0};
                get_str(pobj, psz, "goal",  goal, sizeof(goal));
                if (!goal[0]) get_str(pobj, psz, "label", goal, sizeof(goal));
                if (goal[0]) {
                    copy_str(step->args[0], goal, SANDBOX_ARG_MAX_LEN, goal+sizeof(goal));
                    step->argc = 1;
                }
            }
            break;
        }
        case OP_ECHO: {
            step->argc=1;
            copy_str(step->args[0], name[0]?name:"echo",
                     SANDBOX_ARG_MAX_LEN, name+SANDBOX_ARG_MAX_LEN);
            break;
        }
        default:
            step->argc=0;
            break;
    }
    return true;
}

/* =========================================================================
 * Recursively populate a pipeline from a "steps" array, including
 * then_body/else_body for ConditionalStatements.
 * ========================================================================= */
static void fill_pipeline(pipeline_t *pipe,
                          const char *steps_arr, const char *stmt_end)
{
    const char *sp = steps_arr + 1;
    while (sp < stmt_end && pipe->step_count < PIPELINE_MAX_STEPS) {
        sp = arr_next_obj(sp, stmt_end);
        if (!sp) break;
        const char *se = obj_end(sp, stmt_end);
        if (!se) break;

        pipeline_node_t node;
        if (build_step(sp, se, &node)) {
            uint32_t idx = pipe->step_count++;
            pipe->steps[idx] = node;

            /* If conditional: inline then_body + else_body steps after the OP_IF */
            if (node.opcode == OP_IF) {
                size_t ssz = (size_t)(se - sp);
                /* then_body */
                const char *tarr = find_arr(sp, ssz, "then_body");
                if (tarr) {
                    const char *tp = tarr + 1;
                    while (tp < se && pipe->step_count < PIPELINE_MAX_STEPS) {
                        tp = arr_next_obj(tp, se);
                        if (!tp) break;
                        const char *te = obj_end(tp, se);
                        if (!te) break;
                        pipeline_node_t tn;
                        if (build_step(tp, te, &tn))
                            pipe->steps[pipe->step_count++] = tn;
                        tp = te + 1;
                    }
                }
                /* else_body */
                const char *earr = find_arr(sp, ssz, "else_body");
                if (earr) {
                    const char *ep = earr + 1;
                    while (ep < se && pipe->step_count < PIPELINE_MAX_STEPS) {
                        ep = arr_next_obj(ep, se);
                        if (!ep) break;
                        const char *ee = obj_end(ep, se);
                        if (!ee) break;
                        pipeline_node_t en;
                        if (build_step(ep, ee, &en))
                            pipe->steps[pipe->step_count++] = en;
                        ep = ee + 1;
                    }
                }
            }
        }
        sp = se + 1;
    }
}

/* =========================================================================
 * aiql_extract
 * ========================================================================= */
aiql_err_t aiql_extract(const char *json, size_t len, aiql_program_t *out)
{
    if (!json || !out || !len) return AIQL_ERR_NULL;
    k_memset(out, 0, sizeof(aiql_program_t));

    const char *end = json + len;

    /* intent.goal */
    const char *iobj = find_obj(json, len, "intent");
    if (iobj) {
        const char *ie = obj_end(iobj, end);
        if (ie) get_str(iobj, (size_t)(ie-iobj), "goal", out->goal, sizeof(out->goal));
    }

    /* body array */
    const char *body = find_arr(json, len, "body");
    if (!body) return AIQL_ERR_NOBODY;

    const char *p = body + 1;
    while (p < end && out->pipeline_count < AIQL_MAX_PIPELINES) {
        p = arr_next_obj(p, end);
        if (!p) break;
        const char *se = obj_end(p, end);
        if (!se) break;
        size_t ssz = (size_t)(se - p);

        char stype[32]={0};
        get_str(p, ssz, "type", stype, sizeof(stype));

        if (k_strncmp(stype, "PipelineStatement", 17) == 0) {
            pipeline_t *pipe = &out->pipelines[out->pipeline_count];
            k_memset(pipe, 0, sizeof(pipeline_t));
            const char *sarr = find_arr(p, ssz, "steps");
            if (sarr) fill_pipeline(pipe, sarr, se);
            if (pipe->step_count > 0) out->pipeline_count++;
        }
        /* Other statement types (LoadStatement etc.) silently skipped */

        p = se + 1;
    }

    return (out->pipeline_count > 0) ? AIQL_OK : AIQL_ERR_NOBODY;
}

/* =========================================================================
 * aiql_execute_program
 * ========================================================================= */
sandbox_result_t aiql_execute_program(aiql_program_t *prog,
                                      sandbox_ctx_t  *ctx,
                                      uint32_t        caps __attribute__((unused)))
{
    if (!prog || !ctx) return SANDBOX_ERR_UNKNOWN;

    if (prog->goal[0]) {
        uart_puts("[aiql] goal: ");
        uart_puts(prog->goal);
        uart_puts("\r\n");
    }

    sandbox_result_t last = SANDBOX_OK;
    char nbuf[20];

    for (uint32_t i = 0; i < prog->pipeline_count; i++) {
        pipeline_t *pipe = &prog->pipelines[i];

        uart_puts("[aiql] pipeline ");
        uart_puts(u64_str((uint64_t)(i+1), nbuf));
        uart_puts("/");
        uart_puts(u64_str((uint64_t)prog->pipeline_count, nbuf));
        uart_puts(" (");
        uart_puts(u64_str((uint64_t)pipe->step_count, nbuf));
        uart_puts(" steps)\r\n");

        if (!verifier_check_pipeline(pipe, ctx->caps)) {
            uart_puts("[aiql] verification failed\r\n");
            last = SANDBOX_ERR_VERIFY;
            continue;
        }

        ctx->instruction_count = 0;
        last = interpreter_exec_pipeline(pipe, ctx);

        if (last != SANDBOX_OK) {
            uart_puts("[aiql] error: ");
            uart_puts(sandbox_result_str(last));
            uart_puts("\r\n");
        }
    }
    return last;
}

/* =========================================================================
 * aiql_err_str
 * ========================================================================= */
const char *aiql_err_str(aiql_err_t e)
{
    switch (e) {
        case AIQL_OK:           return "ok";
        case AIQL_ERR_PARSE:    return "parse error";
        case AIQL_ERR_NOBODY:   return "no executable body";
        case AIQL_ERR_OVERFLOW: return "overflow";
        case AIQL_ERR_NULL:     return "null argument";
        default:                return "unknown";
    }
}