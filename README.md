# KAI — Kernel AI

How do you give an AI model meaningful control over a system without giving it arbitrary code execution?

KAI is a formally verified, capability-gated, pipeline-based command language that sits between the AI and the hardware. The AI can only do what the verifier permits, and the verifier runs before any execution. Hardware enforcement (MMU + EL0 isolation) means this guarantee holds even if there is a bug in the interpreter.

---

## Table of Contents

- [Features](#features)
- [Project Layout](#project-layout)
- [Requirements](#requirements)
- [Build & Run](#build-and-run)
- [Shell Commands](#shell-commands)
- [Sandbox Tool Calls](#sandbox-tool-calls)
- [AIQL Pipeline Execution](#aiql-pipeline-execution)
- [Capability Flags](#capability-flags)
- [Safety Design](#safety-design)
- [Hardware Enforcement: MMU + EL0](#hardware-enforcement-mmu--el0)
- [Interrupt-Driven Pipelines (Reflexes)](#interrupt-driven-pipelines-reflexes)
- [KAI Script Compiler](#kai-script-compiler)
- [AIQL Integration](#aiql-integration)
- [KAI as the Robot Brain](#kai-as-the-robot-brain)
- [Author](#author)
- [License](#license)

---

## Features

- **Bare-metal AArch64 boot** — custom `boot.S` entry point, BSS zeroing, stack init
- **PL011 UART driver** — blocking I/O with explicit hardware initialisation
- **Capability-gated syscalls** — every kernel operation checks a per-session bitmask
- **Memory region whitelist** — all memory reads validated against linker-defined regions
- **AI session model** — each shell session carries a capability mask controlling access
- **Hardware-enforced sandbox** — MMU + EL0 drop ensures silicon-level isolation; interpreter bugs cannot reach kernel memory
- **Exception vector table** — VBAR_EL1 routes EL0 faults and hardware IRQs to dedicated handlers
- **Interrupt-driven pipelines** — register a pipeline to a GIC interrupt; AI gets hardware reflexes
- **Lightweight sandbox** — parse → verify → execute pipeline for AI tool calls
- **Text-based tool call parser**
- **Pre-execution AST verifier** (opcode whitelist, argument validation, address checks)
- **Hard instruction count limit** per invocation
- **Isolated scratch buffer** — sandboxed writes never reach kernel memory; scratch page is the only EL0-accessible region
- **AIQL (https://github.com/studiohead/AIQL) pipeline engine** — multi-step pipeline execution derived from the AIQL AST schema
- **New opcodes**: `sleep` (timer-based delay), `introspect` (MMIO map query), `wait_event` (WFE yield stub)
- **KAI Script Compiler** — host-side Python tool compiles `.kai` scripts to pipeline strings

---

## Project Layout

```
kai_os/
├── include/
│   └── kernel/
│       ├── irq.h           # GIC-400 driver and IRQ-to-pipeline binding API
│       ├── memory.h        # Memory region whitelist and sys_mem_* declarations
│       ├── mmio.h          # Typed MMIO read/write helpers
│       ├── mmu.h           # MMU page table types, constants, public API
│       ├── sandbox.h       # Sandbox types, opcodes, pipeline structs, public interface
│       ├── string.h        # Freestanding string utilities
│       ├── syscall.h       # Capability flags and syscall declarations
│       └── uart.h          # UART public interface
├── src/
│   ├── arch/
│   │   └── aarch64/
│   │       ├── boot.S      # Entry point, BSS zero, stack init, EL detection
│   │       └── mmu.S       # MMU enable, EL0 entry/exit, exception vector table
│   ├── lib/
│   │   └── string.c        # k_strcmp, k_strncmp, k_strlen, k_memset
│   ├── sandbox/
│   │   ├── interpreter.c   # Tool call + pipeline parser, variable store, opcode dispatcher
│   │   ├── sandbox.c       # Sandbox init, single-shot execute, pipeline run, result strings
│   │   └── verifier.c      # Pre-execution AST and pipeline validation
│   ├── kernel.c            # kernel_main, AI session, command shell
│   ├── irq.c               # GIC-400 driver, IRQ-to-pipeline dispatch table
│   ├── memory.c            # Memory regions, sys_mem_info, sys_mem_read
│   ├── mmu.c               # Page table construction, identity map
│   ├── syscall.c           # sys_uart_write, sys_uart_hex64
│   └── uart.c              # PL011 UART driver
├── tools/
│   ├── kai_compiler.py     # Host-side .kai script compiler
│   └── examples/
│       ├── obstacle_avoid.kai
│       └── timed_sequence.kai
├── scripts/
│   └── linker.ld           # Memory layout, guard pages, EL0 stack, page tables, scratch
├── Makefile
└── README.md
```

---

## Requirements

| Tool | macOS | Arch Linux |
|---|---|---|
| `aarch64-elf-gcc` | `brew install aarch64-elf-gcc` | `pacman -S aarch64-elf-gcc` |
| `aarch64-elf-binutils` | included with above | `pacman -S aarch64-elf-binutils` |
| `qemu-system-aarch64` | `brew install qemu` | `pacman -S qemu-system-aarch64` |
| Python 3.8+ | system or `brew install python` | `pacman -S python` |

---

## Build and Run

```bash
make              # build/kernel.elf + build/kernel.img
make run          # launch QEMU (MMU + caches active)
make debug-run    # launch QEMU with -d int,mmu,cpu_reset, log to /tmp/qemu_debug.log
make size         # print section sizes + key symbol addresses
make clean        # remove build/
```

---

## Shell Commands

| Command | Description |
|---|---|
| `help` | List all available commands |
| `clear` | Clear the terminal screen |
| `el` | Print current exception level (EL1–EL3) |
| `hex` | Print an example 64-bit hex value |
| `mem` | Print BSS and stack boundary addresses |
| `echo <text>` | Echo text back to the terminal |
| `sandbox <call>` | Parse, verify, and execute a single sandboxed tool call |
| `pipeline <steps>` | Parse, verify, and execute a multi-step AIQL pipeline |
| `irq_init` | Enable CPU-level IRQ delivery (activate hardware reflexes) |
| `irq_bind <num> <pipeline>` | Bind a pipeline to a hardware interrupt number |

---

## Sandbox Tool Calls

The sandbox accepts: `<opcode> [arg0] [arg1]`

| Tool call | Description |
|---|---|
| `nop` | No operation |
| `echo <text>` | Print text to UART (requires `CAP_MMIO`) |
| `read <addr> <len>` | Read bytes from a whitelisted address |
| `write <offset> <value>` | Write one byte to the scratch buffer |
| `info` | Print BSS and stack addresses |
| `el` | Print current exception level |
| `caps` | Print current session capability mask |
| `sleep <ms>` | Busy-wait for N milliseconds (0–10000, uses CNTPCT_EL0) |
| `introspect` | Print whitelisted MMIO address map |
| `wait_event` | Yield via WFE (stub — becomes async in future) |

Single-shot examples:
```
sandbox echo hello
sandbox read 0x40000000 8
sandbox write 0 0xFF
sandbox sleep 500
sandbox introspect
```

---

## AIQL Pipeline Execution

Pipelines are semicolon-separated sequences of tool calls. Each step can bind its result to a named variable using `->`. The entire pipeline is verified before any step executes.

```
pipeline <step1>; <step2>; <step3>
pipeline <step> -> <varname>; <next step>
pipeline if <left> <op> <right> -> then:<N> else:<M>; <then steps...>; <else steps...>
```

Supported operators for `if`: `== != < > <= >=`

Pipeline examples:
```
pipeline el -> level; caps; echo done
pipeline read 0x40000000 4 -> data; echo read complete
pipeline if 1 == 1 -> then:1 else:1; echo condition true; echo condition false
pipeline sleep 250; echo wake; sleep 250; echo done
pipeline introspect; el; caps
```

---

## Capability Flags

| Flag | Value | Grants |
|---|---|---|
| `CAP_NONE` | `0x00` | No capabilities |
| `CAP_READ_MEM` | `0x01` | Read whitelisted memory regions |
| `CAP_WRITE_MEM` | `0x02` | Write to sandbox scratch buffer |
| `CAP_MMIO` | `0x04` | UART output and MMIO access |
| `CAP_SYSTEM` | `0x08` | Privileged system operations |

The default session starts with `CAP_MMIO | CAP_READ_MEM | CAP_UART_WRITE`.

---

## Safety Design

- **BSS zeroing** uses pointer comparison (`__bss_start` vs `__bss_end`) — no word-count drift
- **Stack guard page** — 4 KB unmapped region between `.bss` and kernel stack catches overflows
- **EL0 guard pages** — separate guard pages above and below the EL0 stack
- **`isb` after BSS zero** — prevents CPU reordering of BSS writes before `kernel_main` starts
- **Linker symbols** use `PROVIDE()` so empty `.bss` never causes a link error
- **`/DISCARD/`** strips `.comment`, `.eh_frame`, and ARM unwind tables from the binary
- **Address whitelisting** — `sys_mem_read` validates the entire requested range before copying a single byte
- **Sandbox verifier runs before any execution** — opcode whitelist, argument count check, capability check, and address range validation all happen before `interpreter_exec` is called
- **Pipeline pre-verification** — the entire pipeline is verified before any step executes; a bad step mid-sequence never causes partial execution
- **Scratch buffer isolation** — `OP_WRITE` can only target `ctx->scratch`, never kernel memory
- **Instruction limit** — `SANDBOX_MAX_INSNS` (64) prevents runaway sandbox or pipeline execution
- **Non-printable input** rejected at the shell level before entering any command handler
- **Variable store bounded** — `VAR_STORE_SIZE` (8) caps memory used by pipeline variable bindings

---

## Hardware Enforcement: MMU + EL0

Previous versions enforced the sandbox boundary purely in software (the verifier). KAI now adds a second, silicon-enforced layer.

### Page Table Layout

KAI uses a single flat identity map via TTBR0_EL1 (T0SZ=32, 32-bit VA, 4KB granule, 2MB blocks at L2). The kernel loads and runs at physical address 0x40000000 — a lower 4GB address — so a split VA space (TTBR0/TTBR1) is not used. TTBR1 walks are disabled via `TCR_EL1.EPD1`.

```
TTBR0_EL1 — single identity map covering 0x00000000–0xFFFFFFFF:

  0x08000000  GIC distributor + CPU interface   Device nGnRnE, EL1 RW
  0x09000000  PL011 UART                        Device nGnRnE, EL1 RW
  0x40000000  Kernel image (code, data, stacks,
              page tables) up to __kernel_end   Normal NC, EL1 RWX, EL0 none
  0x40200000  Sandbox scratch (2 MB block)      Normal NC, EL1+EL0 RW, no-exec
```

EL0 access to any address outside the scratch block causes a Translation Fault. The EL1 sync exception handler catches it and returns `SANDBOX_ERR_EL0`.

### Cache Configuration

`mmu_enable_asm` activates the MMU and caches in a single safe sequence:
1. Write MAIR/TCR/TTBR0, flush TLBs
2. Set `SCTLR_EL1.M=1` only — MMU live, caches still off (safe with WB descriptors for this brief window)
3. `IC IALLU` + barriers — invalidate I-cache
4. Set `SCTLR_EL1.C=1` + `SCTLR_EL1.I=1` — D-cache and I-cache now on
5. `TLBI VMALLE1` + barriers

After `mmu_enable()` returns, both caches are active. Memory is mapped Normal Non-Cacheable (NC) — sufficient for correct operation and avoids cache-coherency issues during the MMU enable transition.

### EL0 Drop Model

When `sandbox_execute` or `sandbox_run_pipeline` is called:

1. `el0_enter()` (assembly) saves all callee-saved EL1 registers on the kernel stack
2. Sets `SP_EL0 = __el0_stack_top` (the dedicated EL0 stack region)
3. Sets `SPSR_EL1` for EL0 return and `ELR_EL1` to the sandbox trampoline
4. `eret` drops to EL0 — silicon now enforces the boundary
5. EL0 code executes with access only to the scratch page
6. When done, EL0 issues `SVC #0`
7. The EL1 sync vector catches it, restores registers, returns `sandbox_result_t`

This means a zero-day bug in the interpreter cannot reach kernel memory — the hardware will fault before any out-of-bounds access completes.

### Memory Layout (linker.ld)

```
0x40000000   .text, .rodata, .data, .bss
             [4 KB guard page]
             [64 KB kernel stack ↓]
             [4 KB EL0 guard page]
             [16 KB EL0 sandbox stack ↓]
             [4 KB EL0 stack guard]
             .page_tables   (12 KB — L1 + L2_GB0 (MMIO) + L2_GB1 (kernel))
__kernel_end
             [padding to 2 MB boundary]
0x40200000   .sandbox_scratch (2 MB — EL0+EL1 RW, its own L2 block)
```

---

## Interrupt-Driven Pipelines (Reflexes)

KAI supports binding a pre-verified pipeline to a hardware interrupt. When the interrupt fires, the pipeline executes immediately — no human input required. This is the AI's reflex system.

### Registering a Reflex (from C)

```c
pipeline_t obstacle_pipeline;
interpreter_parse_pipeline(
    "read 0x40000000 4 -> dist; "
    "if dist < 10 -> then:2 else:1; "
    "write 0 0x02; write 1 0x01; "
    "write 0 0x01",
    &obstacle_pipeline
);

// Verify before registration — dispatch skips re-verification for speed
verifier_check_pipeline(&obstacle_pipeline, sb_ctx.caps);

irq_register_pipeline(33, &obstacle_pipeline, &sb_ctx);
irq_enable(33);
```

Then from the shell:
```
irq_init  ← enables CPU-level IRQ delivery
```

After this, GPIO interrupt 33 firing will immediately execute the pipeline.

### GIC-400 Configuration (QEMU -M virt)

| Register | Address | Purpose |
|---|---|---|
| `GICD_CTLR` | `0x08000000` | Distributor enable |
| `GICD_ISENABLER` | `0x08000100` | Per-interrupt enable bits |
| `GICC_CTLR` | `0x08010000` | CPU interface enable |
| `GICC_PMR` | `0x08010004` | Priority mask |
| `GICC_IAR` | `0x0801000C` | Interrupt acknowledge |
| `GICC_EOIR` | `0x08010010` | End of interrupt |

---

## KAI Script Compiler

The host-side compiler (`tools/kai_compiler.py`) converts human-friendly `.kai` scripts to KAI pipeline strings.

### Usage

```bash
# Compile a script and print the pipeline string
python3 tools/kai_compiler.py tools/examples/obstacle_avoid.kai

# Compile and emit as a ready-to-paste KAI shell command
python3 tools/kai_compiler.py tools/examples/obstacle_avoid.kai --run

# List all available opcodes
python3 tools/kai_compiler.py --introspect
```

### .kai Script Syntax

```
# This is a comment
read 0x40000000 4 -> front_sensor
read 0x40000004 4 -> left_sensor

if front_sensor < 10
    then:
        write 0 0x02
        write 1 0x01
        echo reversing
    else:
        write 0 0x01
        echo forward

sleep 500
echo done
```

The compiler handles flattening of `if/then/else` blocks into the wire format (`then:N else:M` step offsets), rejoining `echo` arguments, and validating against known limits before output.

---

## AIQL Integration

KAI's pipeline engine is derived from the AIQL AST schema:

| AIQL Schema Type | KAI OS Implementation |
|---|---|
| `PipelineStatement` | `pipeline_t` — array of `pipeline_node_t` |
| `Operation` | `pipeline_node_t` with `output_var` binding |
| `ConditionalStatement` | `OP_IF` with `then_count` / `else_count` |
| `BinaryExpression` | `pipeline_cond_t` with `cmp_op_t` |
| `Variable` | `var_entry_t` in `var_store_t` |
| `Literal` | `uint64_t` inline in `operand_t` |

Certain AIQL AST types are intentionally omitted (require hosted OS capabilities):
- `CallStatement` (model / visualize)
- `LoadStatement` (requires filesystem / network)

---

## KAI as the Robot Brain

### Cognitive Core

KAI functions as a kernel-level AI brain:

- **Perception**: Sensor inputs are read through verified memory regions (`OP_READ`)
- **Planning**: AIQL pipelines represent thought sequences — multi-step plans with conditional logic
- **Decision Making**: `OP_IF` allows the AI to reason about environment and goals before acting
- **Reflexes**: Interrupt-driven pipelines fire on hardware events without waiting for a command

### Brain Layer Analogy

| Brain Layer | KAI Implementation |
|---|---|
| Brainstem | MMU + capability system — enforces safety, cannot be bypassed |
| Cerebellum | AIQL pipelines — pre-verified motor sequences with timing (`sleep`) |
| Cortex | Higher-level AI running above KAI — generates pipelines, interprets results |

### Example Cognitive Pipeline

```
# Sense → decide → act
read 0x40000000 4 -> front_sensor
read 0x40000004 4 -> left_sensor
if front_sensor < 10 -> then:3 else:1
write 0 0x02         # reverse
write 1 0x01         # turn left
sleep 500            # hold motor command
write 0 0x01         # resume forward
```

### Why This Architecture Is Different

- **Hardware-enforced, not software-trusted**: The MMU and EL privilege system make isolation a silicon guarantee, not a code convention
- **Do not cause harm, not prevent harm**: The policy engine is a refusal system. The AI cannot take an action the verifier forbids. It has no mandate to intervene in the world — only to act within bounds
- **Pre-verification beats post-sandboxing**: A bad pipeline never starts. There is no partial execution, no rollback, no cleanup
- **Minimal latency**: Running at kernel level avoids OS-level bottlenecks and context switching
- **Reflexes without autonomy**: Interrupt-driven pipelines respond to hardware events, but each pipeline was verified before registration. The reflex is fast; the safety check is not skipped

---

## Author

Stephen Johnny Davis

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
