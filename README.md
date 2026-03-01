# KAI OS — Kernel AI Operating System

A minimal kernel targeting the QEMU `virt` board, with a
capability-gated syscall interface and a lightweight AI tool sandbox.

---

## Features

- **Bare-metal AArch64 boot** — custom `boot.S` entry point, BSS zeroing, stack init
- **PL011 UART driver** — blocking I/O with explicit hardware initialisation
- **Capability-gated syscalls** — every kernel operation checks a per-session bitmask
- **Memory region whitelist** — all memory reads validated against linker-defined regions
- **AI session model** — each shell session carries a capability mask controlling access
- **Lightweight sandbox** — parse → verify → execute pipeline for AI tool calls
  - Text-based tool call parser
  - Pre-execution AST verifier (opcode whitelist, argument validation, address checks)
  - Hard instruction count limit per invocation
  - Isolated scratch buffer — sandboxed writes never reach kernel memory

---

## Project Layout

```
kai_os/
├── include/
│   └── kernel/
│       ├── memory.h        # Memory region whitelist and sys_mem_* declarations
│       ├── mmio.h          # Typed MMIO read/write helpers
│       ├── sandbox.h       # Sandbox types, opcodes, and public interface
│       ├── string.h        # Freestanding string utilities
│       ├── syscall.h       # Capability flags and syscall declarations
│       └── uart.h          # UART public interface
├── src/
│   ├── arch/
│   │   └── aarch64/
│   │       └── boot.S      # Entry point, BSS zero, stack init, EL detection
│   ├── lib/
│   │   └── string.c        # k_strcmp, k_strncmp, k_strlen, k_memset
│   ├── sandbox/
│   │   ├── interpreter.c   # Tool call parser and opcode dispatcher
│   │   ├── sandbox.c       # Sandbox init, execute pipeline, result strings
│   │   └── verifier.c      # Pre-execution AST validation
│   ├── kernel.c            # kernel_main, AI session, command shell
│   ├── memory.c            # Memory regions, sys_mem_info, sys_mem_read
│   ├── syscall.c           # sys_uart_write, sys_uart_hex64
│   └── uart.c              # PL011 UART driver
├── scripts/
│   └── linker.ld           # Memory layout, guard page, stack region
├── Makefile
└── README.md
```

---

## Requirements

| Tool                   | macOS                              | Arch Linux                           |
|------------------------|------------------------------------|--------------------------------------|
| `aarch64-elf-gcc`      | `brew install aarch64-elf-gcc`     | `pacman -S aarch64-elf-gcc`          |
| `aarch64-elf-binutils` | included with above                | `pacman -S aarch64-elf-binutils`     |
| `qemu-system-aarch64`  | `brew install qemu`                | `pacman -S qemu-system-aarch64`      |

---

## Build & Run

```sh
make            # build/kernel.elf + build/kernel.img
make run        # launch QEMU in current terminal
make size       # print section sizes
make clean      # remove build/
```

---

## Shell Commands

| Command             | Description                                      |
|---------------------|--------------------------------------------------|
| `help`              | List all available commands                      |
| `clear`             | Clear the terminal screen                        |
| `el`                | Print current exception level (EL1–EL3)          |
| `hex`               | Print an example 64-bit hex value                |
| `mem`               | Print BSS and stack boundary addresses           |
| `echo <text>`       | Echo text back to the terminal                   |
| `sandbox <call>`    | Parse, verify, and execute a sandboxed tool call |

---

## Sandbox Tool Calls

The sandbox accepts a simple text format: `<opcode> [arg0] [arg1]`

| Tool call                  | Description                                          |
|----------------------------|------------------------------------------------------|
| `nop`                      | No operation                                         |
| `echo <text>`              | Print text to UART (requires `CAP_MMIO`)             |
| `read <addr> <len>`        | Read bytes from a whitelisted address                |
| `write <offset> <value>`   | Write one byte to the scratch buffer                 |
| `info`                     | Print BSS and stack addresses                        |
| `el`                       | Print current exception level                        |
| `caps`                     | Print current session capability mask                |

**Examples:**
```
sandbox echo hello
sandbox read 0x40000000 8
sandbox write 0 0xFF
sandbox info
sandbox caps
```

---

## Capability Flags

| Flag           | Value  | Grants                                        |
|----------------|--------|-----------------------------------------------|
| `CAP_NONE`     | `0x00` | No capabilities                               |
| `CAP_READ_MEM` | `0x01` | Read whitelisted memory regions               |
| `CAP_WRITE_MEM`| `0x02` | Write to sandbox scratch buffer               |
| `CAP_MMIO`     | `0x04` | UART output and MMIO access                   |
| `CAP_SYSTEM`   | `0x08` | Privileged system operations                  |

The default session starts with `CAP_MMIO | CAP_READ_MEM`.

---

## Safety Design

- **BSS zeroing** uses pointer comparison (`__bss_start` vs `__bss_end`) — no word-count drift
- **Stack guard page** — 4 KB unmapped region between `.bss` and stack catches overflows
- **Linker symbols** use `PROVIDE()` so empty `.bss` never causes a link error
- **`/DISCARD/`** strips `.comment`, `.eh_frame`, and ARM unwind tables from the binary
- **Address whitelisting** — `sys_mem_read` validates the entire requested range fits within
  a whitelisted region before copying a single byte
- **Sandbox verifier** runs before any execution — opcode whitelist, argument count check,
  capability check, and address range validation all happen before `interpreter_exec` is called
- **Scratch buffer isolation** — `OP_WRITE` can only target `ctx->scratch`, never kernel memory
- **Instruction limit** — `SANDBOX_MAX_INSNS` (32) prevents runaway sandbox execution
- **Non-printable input** rejected at the shell level before entering any command handler

---

## Author

**Stephen Johnny Davis**

---

## License

MIT License

Copyright (c) 2026 Stephen Johnny Davis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
