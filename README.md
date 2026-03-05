# KAI — Kernel AI Operating System

<i>How do you give an AI model meaningful control over a system without giving it arbitrary code execution?</i>
 
KAI is a formally verified, capability-gated, pipeline-based command language that sits between the AI and the hardware. The AI can only do what the verifier permits, and the verifier runs before any execution. This is architecturally cleaner than sandboxing approaches that restrict after the fact.

# Table of Contents

- [KAI — Kernel AI Operating System](#kai-—-kernel-ai-operating-system)
- [Features](#features)
- [Project Layout](#project-layout)
- [Requirements](#requirements)
- [Build & Run](#build-and-run)
- [Shell Commands](#shell-commands)
- [Sandbox Tool Calls](#sandbox-tool-calls)
- [AIQL Pipeline Execution](#aiql-pipeline-execution)
- [Capability Flags](#capability-flags)
- [Safety Design](#safety-design)
- [AIQL Integration](#aiql-integration)
- [KAI as the Robot Brain](#kai-as-the-robot-brain)
  - [1. Cognitive Core](#1-cognitive-core)
  - [2. Perception → Thought → Action](#2-perception-→-thought-→-action)
  - [3. Memory and Variable Store](#3-memory-and-variable-store)
  - [4. Conditional Planning](#4-conditional-planning)
  - [5. Safe Motor Commands](#5-safe-motor-commands)
  - [6. Example Cognitive Pipeline](#6-example-cognitive-pipeline)
  - [7. Why This is Unique](#7-why-this-is-unique)
- [Author](#author)
- [License](#license)

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
- **AIQL pipeline engine** — multi-step pipeline execution derived from the AIQL/PIQL AST schema
  - Semicolon-separated steps with `->` output variable bindings
  - Named variable store persisting across pipeline steps
  - Conditional branching via `OP_IF` (maps to AIQL `ConditionalStatement`)
  - Full pipeline pre-verification before any step executes

---

## Project Layout

```
kai_os/
├── include/
│   └── kernel/
│       ├── memory.h        # Memory region whitelist and sys_mem_* declarations
│       ├── mmio.h          # Typed MMIO read/write helpers
│       ├── sandbox.h       # Sandbox types, opcodes, pipeline structs, public interface
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
│   │   ├── interpreter.c   # Tool call + pipeline parser, variable store, opcode dispatcher
│   │   ├── sandbox.c       # Sandbox init, single-shot execute, pipeline run, result strings
│   │   └── verifier.c      # Pre-execution AST and pipeline validation
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

## Build and Run

```sh
make            # build/kernel.elf + build/kernel.img
make run        # launch QEMU in current terminal
make size       # print section sizes
make clean      # remove build/
```

---

## Shell Commands

| Command               | Description                                             |
|-----------------------|---------------------------------------------------------|
| `help`                | List all available commands                             |
| `clear`               | Clear the terminal screen                               |
| `el`                  | Print current exception level (EL1–EL3)                 |
| `hex`                 | Print an example 64-bit hex value                       |
| `mem`                 | Print BSS and stack boundary addresses                  |
| `echo <text>`         | Echo text back to the terminal                          |
| `sandbox <call>`      | Parse, verify, and execute a single sandboxed tool call |
| `pipeline <steps>`    | Parse, verify, and execute a multi-step AIQL pipeline   |

---

## Sandbox Tool Calls

The sandbox accepts a simple text format: `<opcode> [arg0] [arg1]`

| Tool call                | Description                                          |
|--------------------------|------------------------------------------------------|
| `nop`                    | No operation                                         |
| `echo <text>`            | Print text to UART (requires `CAP_MMIO`)             |
| `read <addr> <len>`      | Read bytes from a whitelisted address                |
| `write <offset> <value>` | Write one byte to the scratch buffer                 |
| `info`                   | Print BSS and stack addresses                        |
| `el`                     | Print current exception level                        |
| `caps`                   | Print current session capability mask                |

**Single-shot examples:**
```
sandbox echo hello
sandbox read 0x40000000 8
sandbox write 0 0xFF
sandbox info
sandbox caps
```

---

## AIQL Pipeline Execution

Pipelines are semicolon-separated sequences of tool calls. Each step can bind
its result to a named variable using `->`, which subsequent steps can reference.
The entire pipeline is verified before any step executes.

**Format:**
```
pipeline <step1>; <step2>; <step3>
pipeline <step> -> <varname>; <next step>
pipeline if <left> <op> <right> -> then:<N> else:<M>; <then steps...>; <else steps...>
```

**Supported operators for `if`:** `==` `!=` `<` `>` `<=` `>=`

**Pipeline examples:**
```
pipeline el -> level; caps; echo done
pipeline read 0x40000000 4 -> data; echo read complete
pipeline if 1 == 1 -> then:1 else:1; echo condition true; echo condition false
pipeline nop; echo step one; echo step two; echo step three
```

---

## Capability Flags

| Flag            | Value  | Grants                                        |
|-----------------|--------|-----------------------------------------------|
| `CAP_NONE`      | `0x00` | No capabilities                               |
| `CAP_READ_MEM`  | `0x01` | Read whitelisted memory regions               |
| `CAP_WRITE_MEM` | `0x02` | Write to sandbox scratch buffer               |
| `CAP_MMIO`      | `0x04` | UART output and MMIO access                   |
| `CAP_SYSTEM`    | `0x08` | Privileged system operations                  |

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
- **Pipeline pre-verification** — the entire pipeline is verified before any step executes;
  a bad step mid-sequence never causes partial execution
- **Scratch buffer isolation** — `OP_WRITE` can only target `ctx->scratch`, never kernel memory
- **Instruction limit** — `SANDBOX_MAX_INSNS` (64) prevents runaway sandbox or pipeline execution
- **Non-printable input** rejected at the shell level before entering any command handler
- **Variable store bounded** — `VAR_STORE_SIZE` (8) caps memory used by pipeline variable bindings

---

## AIQL Integration

KAI's pipeline engine is derived from the **AIQL/PIQL AST schema**, providing unique control over safe AI execution:  

| AIQL Schema Type        | KAI OS Implementation                          |
|-------------------------|-----------------------------------------------|
| PipelineStatement       | `pipeline_t` — array of `pipeline_node_t`    |
| Operation               | `pipeline_node_t` with `output_var` binding  |
| ConditionalStatement    | `OP_IF` with `then_count` / `else_count`    |
| BinaryExpression        | `pipeline_cond_t` with `cmp_op_t`           |
| Variable                | `var_entry_t` in `var_store_t`               |
| Literal                 | `uint64_t` inline in `operand_t`            |

**What AIQL enables that KAI cannot do otherwise:**

- Multi-step pipelines with **pre-execution verification**  
- Step-wise **result binding** to named variables for later steps  
- Conditional branching fully checked **before execution**  
- Ensures AI cannot partially run dangerous sequences  
- Integrates directly with kernel-level capabilities while **maintaining strict safety boundaries**  

Certain AIQL AST types are intentionally omitted (require hosted OS capabilities):

- CallStatement (model / visualize)  
- LoadStatement (requires filesystem / network)  

---

## KAI as the Robot Brain

### 1. Cognitive Core

KAI functions like a kernel-level AI brain, where:

- **Perception:** Sensor inputs (distance, force, temperature) are read through verified memory regions.
- **Planning:** AIQL pipelines represent “thought sequences” — multi-step plans with conditional logic.
- **Decision Making:** Conditional branching allows AI to reason about environment, goals, and safety before acting.

Basic human brain parallels:

- Brainstem: enforces safety, basic reflexes, raw motor execution → KAI’s verified, capability-gated hardware control.

- Cerebellum / basal ganglia: handles planning sequences, timing, and smooth execution → AIQL pipelines with pre-verified multi-step plans.

- Cortex: higher reasoning, abstract thought → what a higher-level AI might implement on top of KAI.

**Analogy:** Each AIQL pipeline is like a thought process or motor plan in a human brain. Variables store memory of previous sensory data or intermediate calculations.

---

### 2. Perception → Thought → Action

**Flow:**

**Perception:** Sensor readings are loaded into variables:

```
read 0x40000000 4 -> front_sensor
read 0x40000004 4 -> left_sensor
```


**Internal Reasoning:** AIQL operations compute values or conditions:

```
if front_sensor < 10 -> then:2 else:1
sensor_average = (front_sensor + left_sensor) / 2
```

**Decision / Action:** Outputs trigger actuator commands via verified capability calls:

```
write 0 0x02 # Move backward
write 1 0x01 # Turn left
```

Each step is **pre-verified**, so the AI can plan multi-step sequences **without risk of unsafe execution**.

---

### 3. Memory and Variable Store

- **Working Memory:** AIQL’s variable store acts like short-term memory for the robot brain.
  - Each pipeline can store intermediate sensor readings, computations, or sub-plan results.
  
**Example:**

```
read 0x40001000 4 -> torque
torque -> next_step if torque > threshold
```

- **Persistent Short-Term Memory:** Variables persist only for the pipeline duration, enforcing controlled cognition without allowing uncontrolled state mutations.

---

### 4. Conditional Planning

- Conditional execution allows the AI to **reason about multiple outcomes** before acting:

```
if obstacle_detected -> then:2 else:1
```

- Branches map directly to safety or navigation choices.
- AI can plan multi-step contingencies while remaining fully bounded by the **sandbox and capabilities**.

---

### 5. Safe Motor Commands

- AI’s “muscles” are **capability-gated hardware writes**.
  - Example: `CAP_MMIO` allows UART or motor register writes.
- Sandbox ensures:
  - Writes are restricted to **safe ranges**.
  - Instruction count limits prevent runaway loops.
  - No memory outside `ctx->scratch` or whitelisted MMIO regions is accessible.

This ensures the **brain cannot override hardware safety rules**, even if AI miscalculates.

---

### 6. Example Cognitive Pipeline

```
pipeline read 0x40000000 4 -> front_sensor;
read 0x40000004 4 -> left_sensor;
if front_sensor < 10 -> then:2 else:1;
write 0 0x02; # Move backward
write 1 0x01; # Turn left
echo step complete
```

- **Step 1:** Sense environment.
- **Step 2:** Decide whether a corrective action is required.
- **Step 3:** Execute motor commands.
- **Step 4:** Feedback via `echo` (like reporting internal state).

---

### 7. Why This is Unique

- **Formal pre-verification:** Every AI decision sequence is checked **before touching hardware**.
- **Pipeline as thought:** Multi-step sequences with variable memory and conditional logic mimic a brain’s planning process.
- **Bounded yet flexible:** AI can adapt to new inputs and recompute decisions without ever breaching safety.
- **Minimal latency:** Running AIQL at kernel level avoids OS-level bottlenecks and context switching.

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
