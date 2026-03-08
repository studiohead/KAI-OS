/*
 * src/kernel.c — Kernel main entry point and command shell with AI session support
 *
 * kernel_main() is called by boot.S after the stack and BSS are ready.
 * Hardware drivers (UART, etc.) are fully separated into their own
 * translation units; only their public headers are included here.
 *
 * This version adds AI session awareness:
 * - Each session has a capabilities mask controlling which syscalls are allowed
 * - Command handlers receive a session pointer
 * - Syscalls now use session->caps instead of fixed SYS_CALL_CAPS
 */

/* ---- Standard includes & helper macros -------------------------------- */
#include <kernel/sandbox.h>
#include <kernel/intent.h>
#include <kernel/memory.h>
#include <kernel/mmu.h>
#include <kernel/irq.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* * Capability Bitmask Definitions 
 * These ensure the ai_session_t initialization below compiles and 
 * the verifier can correctly gate hardware access.
 */
#ifndef CAP_MMIO
#define CAP_MMIO       (1U << 0)
#endif
#ifndef CAP_READ_MEM
#define CAP_READ_MEM   (1U << 1)
#endif
#ifndef CAP_UART_WRITE
#define CAP_UART_WRITE (1U << 3)
#endif

/* External assembly function from src/arch/aarch64/mmu.S */
extern void vbar_install(void);

/* Helper to get string length for sys_uart_write without hardcoding */
#define KSTRLEN(str) ((size_t)k_strlen(str))

/* ---- Configuration --------------------------------------------------- */
#define CMD_BUF_SIZE    128U    /* Max bytes per command line (incl. NUL) */
#define PROMPT          "m4-kernel# "

/* ---- AI Session Struct ----------------------------------------------- */
typedef struct {
    uint32_t caps;                 /* Allowed syscalls / capabilities */
} ai_session_t;

/* ---- Sandbox context (global so irq_dispatch can access it) ---------- */
/* Using the global context allows hardware interrupts to share state 
 * with the user shell, enabling 'reflexes' that the user can monitor. */
static sandbox_ctx_t sb_ctx;

/* ---- Command typedef must come first --------------------------------- */
typedef struct {
    const char *name;
    const char *help;
    void (*handler)(const char *args, ai_session_t *session);
} command_t;

/* ---- Private helpers ------------------------------------------------- */

/* Read the current Exception Level (EL1–EL3) */
static uint32_t current_el(void)
{
    uint64_t el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r" (el));
    return (uint32_t)((el >> 2U) & 3U);
}

/* Returns true if the character is printable (safe to echo/store) */
static bool is_printable(char c)
{
    return (unsigned char)c >= 0x20U && (unsigned char)c < 0x7FU;
}

/* ---- Forward declarations of handlers -------------------------------- */
static void cmd_help(const char *args, ai_session_t *session);
static void cmd_clear(const char *args, ai_session_t *session);
static void cmd_el(const char *args, ai_session_t *session);
static void cmd_hex(const char *args, ai_session_t *session);
static void cmd_mem(const char *args, ai_session_t *session);
static void cmd_echo(const char *args, ai_session_t *session);
static void cmd_sandbox(const char *args, ai_session_t *session);
static void cmd_pipeline(const char *args, ai_session_t *session);
static void cmd_irq(const char *args, ai_session_t *session);
static void cmd_irq_bind(const char *args, ai_session_t *session);

/* ---- Command table --------------------------------------------------- */
static const command_t commands[] = {
    { "help",     "Show this help text",                     cmd_help     },
    { "clear",    "Clear the terminal screen",               cmd_clear    },
    { "el",       "Print current exception level",           cmd_el       },
    { "hex",      "Print an example hex value",              cmd_hex      },
    { "mem",      "Print BSS and stack addresses",           cmd_mem      },
    { "echo",     "Echo text back to the terminal",          cmd_echo     },
    { "sandbox",  "Run a sandboxed tool call",               cmd_sandbox  },
    { "pipeline", "Run a multi-step AIQL pipeline",          cmd_pipeline },
    { "irq_init", "Enable CPU IRQ delivery (start reflexes)", cmd_irq      },
    { "irq_bind", "Bind IRQ <num> to <pipeline>",            cmd_irq_bind },
};

#define NUM_COMMANDS  (sizeof(commands) / sizeof(commands[0]))

/* ---- Command Handlers ------------------------------------------------ */

/* Clear terminal screen */
static void cmd_clear(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("\033[2J\033[H", KSTRLEN("\033[2J\033[H"), session->caps);
}

/* Print current exception level */
static void cmd_el(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Current privilege level: EL", 27, session->caps);

    char el_char = '0' + (char)current_el();
    sys_uart_write(&el_char, 1, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Print example 64-bit hex value */
static void cmd_hex(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Example 64-bit hex value: ", KSTRLEN("Example 64-bit hex value: "), session->caps);
    sys_uart_hex64(0xDEADBEEFCAFEBABEULL, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Print memory info via syscalls */
static void cmd_mem(const char *args, ai_session_t *session)
{
    (void)args;
    uintptr_t bss_start, bss_end, stack_start, stack_end;

    if (sys_mem_info(&bss_start, &bss_end, &stack_start, &stack_end, session->caps) == 0) {
        sys_uart_write("BSS start   : ", KSTRLEN("BSS start   : "), session->caps);
        sys_uart_hex64(bss_start, session->caps);
        sys_uart_write("\r\nBSS end     : ", KSTRLEN("\r\nBSS end     : "), session->caps);
        sys_uart_hex64(bss_end, session->caps);
        sys_uart_write("\r\nStack start : ", KSTRLEN("\r\nStack start : "), session->caps);
        sys_uart_hex64(stack_start, session->caps);
        sys_uart_write("\r\nStack end   : ", KSTRLEN("\r\nStack end   : "), session->caps);
        sys_uart_hex64(stack_end, session->caps);
        sys_uart_write("\r\n", 2, session->caps);
    } else {
        sys_uart_write("Memory info not allowed\r\n", KSTRLEN("Memory info not allowed\r\n"), session->caps);
    }
}

/* Echo command */
static void cmd_echo(const char *args, ai_session_t *session)
{
    if (!args) return;
    size_t len = k_strlen(args);
    if (len > CMD_BUF_SIZE - 1) len = CMD_BUF_SIZE - 1;
    sys_uart_write(args, len, session->caps);
    sys_uart_write("\r\n", 2, session->caps);
}

/* Run verified sandbox tool */
static void cmd_sandbox(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: sandbox <command> [args]\r\n", KSTRLEN("usage: sandbox <command> [args]\r\n"), session->caps);
        return;
    }
    sandbox_execute(&sb_ctx, args);
}

/* Run multi-step AIQL pipeline */
static void cmd_pipeline(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: pipeline <step1>; <step2>; ...\r\n", KSTRLEN("usage: pipeline <step1>; <step2>; ...\r\n"), session->caps);
        return;
    }
    sandbox_run_pipeline(&sb_ctx, args);
}

/* Master IRQ Enable */
static void cmd_irq(const char *args, ai_session_t *session)
{
    (void)args;
    irq_enable_in_cpu();
    sys_uart_write("IRQ dispatch enabled. PSTATE.I unmasked.\r\n", KSTRLEN("IRQ dispatch enabled. PSTATE.I unmasked.\r\n"), session->caps);
}

/* Bind an AI reflex to hardware */
static void cmd_irq_bind(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: irq_bind <num> <pipeline>\r\n", KSTRLEN("usage: irq_bind <num> <pipeline>\r\n"), session->caps);
        return;
    }

    uint32_t irq_num = 0;
    const char *p = args;
    while (*p >= '0' && *p <= '9') {
        irq_num = irq_num * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ') p++;

    static pipeline_t reflex_pipe; 
    if (interpreter_parse_pipeline(p, &reflex_pipe)) {
        if (irq_register_pipeline(irq_num, &reflex_pipe, &sb_ctx)) {
            irq_enable(irq_num);
            sys_uart_write("IRQ bound and enabled.\r\n", KSTRLEN("IRQ bound and enabled.\r\n"), session->caps);
        }
    } else {
        sys_uart_write("Error parsing pipeline.\r\n", KSTRLEN("Error parsing pipeline.\r\n"), session->caps);
    }
}

/* Help list */
static void cmd_help(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Available commands:\r\n", KSTRLEN("Available commands:\r\n"), session->caps);
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        sys_uart_write("  ", 2, session->caps);
        sys_uart_write(commands[i].name, k_strlen(commands[i].name), session->caps);
        sys_uart_write("\t- ", KSTRLEN("\t- "), session->caps);
        sys_uart_write(commands[i].help, k_strlen(commands[i].help), session->caps);
        sys_uart_write("\r\n", 2, session->caps);
    }
}

/* ---- Command dispatcher ---------------------------------------------- */
static void execute_command(const char *input, ai_session_t *session)
{
    sys_uart_write("\r\n", 2, session->caps);
    if (input[0] == '\0') return;

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        size_t name_len = k_strlen(commands[i].name);
        if ((k_strncmp(input, commands[i].name, name_len) == 0) &&
            (input[name_len] == '\0' || input[name_len] == ' ')) {
            const char *args = input + name_len;
            while (*args == ' ') args++;
            commands[i].handler(args, session);
            return;
        }
    }
    sys_uart_write("Unknown command.\r\n", KSTRLEN("Unknown command.\r\n"), session->caps);
}

/* ---- Entry point ----------------------------------------------------- */
void kernel_main(void)
{
    /* 1. UART Init */
    uart_init();

    /* 2. MMU Init */
    mmu_init();
    mmu_enable();

    /* 3. Exception Vectors (VBAR) */
    /* Sync with mmu.S: Using vbar_install to load kai_vector_table */
    vbar_install(); 

    /* 4. GIC Init */
    irq_init();

    /* 5. AI Session & Sandbox Preparation 
     * NOTE: If the kernel hangs here, the MMU mapping for the 
     * sandbox scratchpad (0x40400000) is likely missing.
     */
    ai_session_t session = { .caps = CAP_MMIO | CAP_READ_MEM | CAP_UART_WRITE };
    intent_object_t intent = {
        .caps = session.caps,
        .instruction_budget = 1000,
        .pipeline = NULL
    };
    sandbox_init(&sb_ctx, &intent);

    /* 6. Welcome Banner */
    uart_puts("\r\n========================\r\n");
    uart_puts("      Kernel AI OS      \r\n");
    uart_puts("========================\r\n");

    uart_puts("EL: ");
    char el_char = '0' + (char)current_el();
    uart_putc(el_char);
    uart_puts("   |   Ready\r\n\r\n");

    uart_puts(PROMPT);

    /* 7. Main Shell Loop */
    char buf[CMD_BUF_SIZE];
    size_t index = 0U;

    while (true) {
        char c = uart_getc();

        /* Handle Enter/Newline */
        if (c == '\r' || c == '\n') {
            buf[index] = '\0';
            if (index > 0U) {
                execute_command(buf, &session);
                index = 0U;
            }
            uart_puts("\r\n");
            uart_puts(PROMPT);
        }
        /* Handle Backspace (\x7F or \x08) */
        else if ((c == '\x7F') || (c == '\x08')) {
            if (index > 0U) {
                index--;
                uart_puts("\b \b");
            }
        }
        /* Handle Printable Characters */
        else if (is_printable(c) && index < (CMD_BUF_SIZE - 1U)) {
            buf[index++] = c;
            uart_putc(c);
        }
    }
}