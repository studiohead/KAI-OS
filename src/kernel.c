/*
 * src/kernel.c — Kernel main entry point and command shell with AI session support
 *
 * kernel_main() is called by boot.S after the stack and BSS are ready.
 * Hardware drivers (UART, etc.) are fully separated into their own
 * translation units; only their public headers are included here.
 *
 * This version adds AI session awareness:
 *  - Each session has a capabilities mask controlling which syscalls are allowed
 *  - Command handlers receive a session pointer
 *  - Syscalls now use session->caps instead of fixed SYS_CALL_CAPS
 */

/* ---- Standard includes & helper macros -------------------------------- */
#include <kernel/memory.h>
#include <kernel/sandbox.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/uart.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Helper to get string length for sys_uart_write without hardcoding */
#define KSTRLEN(str) ((size_t)k_strlen(str))

/* ---- Configuration --------------------------------------------------- */
#define CMD_BUF_SIZE    128U    /* Max bytes per command line (incl. NUL) */
#define PROMPT          "m4-kernel# "

/* ---- AI Session Struct ----------------------------------------------- */
typedef struct {
    uint32_t caps;                 /* Allowed syscalls / capabilities */
} ai_session_t;

/* ---- Sandbox context (file-scope so cmd_sandbox can access it) ------- */
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

/* ---- Command table --------------------------------------------------- */
static const command_t commands[] = {
    { "help",  "Show this help text",             cmd_help  },
    { "clear", "Clear the terminal screen",       cmd_clear },
    { "el",    "Print current exception level",   cmd_el    },
    { "hex",   "Print an example hex value",      cmd_hex   },
    { "mem",   "Print BSS and stack addresses",   cmd_mem   },
    { "echo",  "Echo text back to the terminal",  cmd_echo  },
    { "sandbox", "Run a sandboxed tool call", cmd_sandbox },
    { "pipeline", "Run a multi-step AIQL pipeline", cmd_pipeline },
};

#define NUM_COMMANDS  (sizeof(commands) / sizeof(commands[0]))

/* ---- Command Handlers ------------------------------------------------ */

/* Clear terminal screen */
static void cmd_clear(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("\033[2J\033[H", KSTRLEN("\033[2J\033[H"), session->caps);
}

/* Print current exception level (freestanding, no snprintf) */
static void cmd_el(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Current privilege level: EL", 27, session->caps);

    char el_char = '0' + (char)current_el();
    sys_uart_write(&el_char, 1, session->caps);
    sys_uart_write("\n", 1, session->caps);
}

/* ---- Print example 64-bit hex value (freestanding, sandbox-safe) ---- */
static void cmd_hex(const char *args, ai_session_t *session)
{
    (void)args;

    sys_uart_write("Example 64-bit hex value: ", KSTRLEN("Example 64-bit hex value: "), session->caps);
    sys_uart_hex64(0xDEADBEEFCAFEBABEULL, session->caps);
    sys_uart_write("\n", 1, session->caps);
}

/* ---- Print memory info via syscalls, sandbox-safe ---- */
static void cmd_mem(const char *args, ai_session_t *session)
{
    (void)args;
    uintptr_t bss_start, bss_end, stack_start, stack_end;

    if (sys_mem_info(&bss_start, &bss_end, &stack_start, &stack_end, session->caps) == 0) {
        sys_uart_write("BSS start   : ", KSTRLEN("BSS start   : "), session->caps);
        sys_uart_hex64(bss_start, session->caps);
        sys_uart_write("\nBSS end     : ", KSTRLEN("\nBSS end     : "), session->caps);
        sys_uart_hex64(bss_end, session->caps);
        sys_uart_write("\nStack start : ", KSTRLEN("\nStack start : "), session->caps);
        sys_uart_hex64(stack_start, session->caps);
        sys_uart_write("\nStack end   : ", KSTRLEN("\nStack end   : "), session->caps);
        sys_uart_hex64(stack_end, session->caps);
        sys_uart_write("\n", 1, session->caps);
    } else {
        sys_uart_write("Memory info not allowed\n", KSTRLEN("Memory info not allowed\n"), session->caps);
    }
}

/* Echo command: prints arguments back safely */
static void cmd_echo(const char *args, ai_session_t *session)
{
    if (!args) return;
    size_t len = k_strlen(args);
    if (len > CMD_BUF_SIZE - 1) len = CMD_BUF_SIZE - 1;
    sys_uart_write(args, len, session->caps);
    sys_uart_write("\n", 1, session->caps);
}

/* Sandbox command: parse, verify, and execute a sandboxed tool call */
static void cmd_sandbox(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: sandbox <command> [args]\n",
                       KSTRLEN("usage: sandbox <command> [args]\n"),
                       session->caps);
        return;
    }

    sandbox_execute(&sb_ctx, args);
}

static void cmd_pipeline(const char *args, ai_session_t *session)
{
    if (!args || args[0] == '\0') {
        sys_uart_write("usage: pipeline <step1>; <step2>; ...\n",
                       KSTRLEN("usage: pipeline <step1>; <step2>; ...\n"),
                       session->caps);
        sys_uart_write("  e.g: pipeline el -> level; echo done\n",
                       KSTRLEN("  e.g: pipeline el -> level; echo done\n"),
                       session->caps);
        return;
    }

    sandbox_run_pipeline(&sb_ctx, args);
}

/* Print help for all commands (ASCII dash to avoid UTF-8 mismatch) */
static void cmd_help(const char *args, ai_session_t *session)
{
    (void)args;
    sys_uart_write("Available commands:\n", KSTRLEN("Available commands:\n"), session->caps);
    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        sys_uart_write("  ", 2, session->caps);
        sys_uart_write(commands[i].name, k_strlen(commands[i].name), session->caps);
        sys_uart_write("\t- ", KSTRLEN("\t- "), session->caps);
        sys_uart_write(commands[i].help, k_strlen(commands[i].help), session->caps);
        sys_uart_write("\n", 1, session->caps);
    }
}

/* ---- Command dispatcher ---------------------------------------------- */
/* Splits input into command and optional arguments, passing session */
static void execute_command(const char *input, ai_session_t *session)
{
    sys_uart_write("\n", 1, session->caps);

    if (input[0] == '\0') {
        sys_uart_write(PROMPT, KSTRLEN(PROMPT), session->caps);
        return;
    }

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        size_t name_len = k_strlen(commands[i].name);

        /* Command matches exactly or is followed by a space (argument) */
        if ((k_strncmp(input, commands[i].name, name_len) == 0) &&
            (input[name_len] == '\0' || input[name_len] == ' ')) {

            const char *args = input + name_len;
            if (*args == ' ') args++;

            commands[i].handler(args, session);
            sys_uart_write(PROMPT, KSTRLEN(PROMPT), session->caps);
            return;
        }
    }

    sys_uart_write("Unknown command: '", KSTRLEN("Unknown command: '"), session->caps);
    sys_uart_write(input, k_strlen(input), session->caps);
    sys_uart_write("'  (type 'help' for a list)\n", KSTRLEN("'  (type 'help' for a list)\n"), session->caps);
    sys_uart_write(PROMPT, KSTRLEN(PROMPT), session->caps);
}

/* ---- Entry point ----------------------------------------------------- */
void kernel_main(void)
{
    uart_init();

    ai_session_t session = {
        .caps = CAP_MMIO | CAP_READ_MEM,
    };

    sandbox_init(&sb_ctx, session.caps);

    /* Normal banner */
    sys_uart_puts("========================\n", session.caps);
    sys_uart_puts("      Kernel AI OS      \n", session.caps);
    sys_uart_puts("========================\n", session.caps);

    sys_uart_puts("EL: ", session.caps);
    char el_char = '0' + (char)current_el();
    sys_uart_puts(&el_char, session.caps);
    sys_uart_puts("   |   Type 'help' for commands\n",
                   session.caps);

    sys_uart_write(PROMPT, KSTRLEN(PROMPT), session.caps);

    /* ---- Main input loop ---- */
    char buf[CMD_BUF_SIZE];
    size_t index = 0U;

    while (true) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            buf[index] = '\0';
            execute_command(buf, &session);
            index = 0U;
        }
        else if ((c == '\x7F') || (c == '\x08')) {
            if (index > 0U) {
                index--;
                sys_uart_write("\b \b", KSTRLEN("\b \b"), session.caps);
            }
        }
        else if (is_printable(c) && index < (CMD_BUF_SIZE - 1U)) {
            buf[index++] = c;
            sys_uart_write(&c, 1, session.caps);
        }
    }
}