# =============================================================================
# Makefile — KAI OS
#
# Requirements:
#   aarch64-elf-gcc   (cross-compiler, e.g. from Homebrew or AUR)
#   aarch64-elf-ld    (linker, part of the same toolchain)
#   aarch64-elf-objcopy
#   qemu-system-aarch64
#
# Targets:
#   all        — build ELF + raw binary image (default)
#   run        — launch QEMU interactively in the current terminal
#   run-agent  — launch QEMU headlessly with serial on /tmp/kai.sock
#                (used by tools/kai_agent.py)
#   clean      — remove all build artefacts
#   size       — print section sizes
# =============================================================================

# ---- Toolchain -------------------------------------------------------------
CC      := aarch64-elf-gcc
LD      := aarch64-elf-ld
OBJCOPY := aarch64-elf-objcopy
SIZE    := aarch64-elf-size

# ---- Directories -----------------------------------------------------------
SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build

# ---- Flags -----------------------------------------------------------------
# -ffreestanding:  The standard library might not exist.
# -nostdlib:       Don't use the standard system libraries.
# -fno-stack-protector: Disable stack smashing protection (requires kernel support).
# --no-warn-rwx-segments: Suppress warnings for RWX segments common in kernels.
CFLAGS := \
    -std=c11                \
    -Wall                   \
    -Wextra                 \
    -Wpedantic              \
    -ffreestanding          \
    -nostdlib               \
    -nostartfiles           \
    -fno-stack-protector    \
    -O2                     \
    -I$(INC_DIR)

LDFLAGS := \
    -T scripts/linker.ld    \
    --no-warn-rwx-segments

# ---- Sources ----------------------------------------------------------------
ASM_SRCS := $(SRC_DIR)/arch/aarch64/boot.S \
             $(SRC_DIR)/arch/aarch64/mmu.S

C_SRCS   := \
    $(SRC_DIR)/kernel.c                  \
    $(SRC_DIR)/uart.c                    \
    $(SRC_DIR)/syscall.c                 \
    $(SRC_DIR)/memory.c                  \
    $(SRC_DIR)/mmu.c                     \
    $(SRC_DIR)/irq.c                     \
    $(SRC_DIR)/kai_node.c                \
    $(SRC_DIR)/kai_interner.c            \
    $(SRC_DIR)/kai_dag.c                 \
    $(SRC_DIR)/kai_scheduler.c           \
    $(SRC_DIR)/aiql.c                    \
    $(SRC_DIR)/sandbox/sandbox.c         \
    $(SRC_DIR)/sandbox/interpreter.c     \
    $(SRC_DIR)/sandbox/verifier.c        \
    $(SRC_DIR)/lib/string.c

# ---- Objects (mirrors source tree under build/) ----------------------------
ASM_OBJS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(ASM_SRCS))
C_OBJS   := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
ALL_OBJS := $(ASM_OBJS) $(C_OBJS)

# ---- Outputs ---------------------------------------------------------------
ELF := $(BUILD_DIR)/kernel.elf
IMG := $(BUILD_DIR)/kernel.img

# ---- QEMU ------------------------------------------------------------------
QEMU := qemu-system-aarch64

# Interactive: serial → current terminal
QEMU_FLAGS := \
    -M virt                 \
    -cpu max                \
    -nographic              \
    -kernel $(ELF)

# Agent mode: kernel runs headlessly, serial exposed on a Unix socket.
# kai_agent.py connects to /tmp/kai.sock to send pipelines and read output.
# Stop with: pkill -f "qemu.*kai.sock"
QEMU_AGENT_FLAGS := \
    -M virt                                     \
    -cpu max                                    \
    -kernel $(ELF)                              \
    -serial unix:/tmp/kai.sock,server,nowait    \
    -monitor none                               \
    -display none

# ============================================================================
.PHONY: all run run-agent clean size

# Default target
all: $(ELF) $(IMG)

# ---- Link ------------------------------------------------------------------
$(ELF): $(ALL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $^ -o $@

# ---- Flat binary -----------------------------------------------------------
$(IMG): $(ELF)
	$(OBJCOPY) -O binary $< $@

# ---- Compile C sources -----------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Assemble ASM sources --------------------------------------------------
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Interactive run -------------------------------------------------------
run: all
	$(QEMU) $(QEMU_FLAGS)

# ---- Agent run (headless, socket-attached) ---------------------------------
# Starts QEMU in the background. Run kai_agent.py in another terminal.
run-agent: all
	@rm -f /tmp/kai.sock
	@echo "  KAI kernel starting (headless, socket: /tmp/kai.sock)"
	@echo "  Run: python3 tools/kai_agent.py --goal \"your goal here\""
	@echo "  Stop: pkill -f 'qemu.*kai.sock'"
	$(QEMU) $(QEMU_AGENT_FLAGS) 2>/dev/null &

# ---- Section size report ---------------------------------------------------
size: $(ELF)
	$(SIZE) $

# ---- Clean -----------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)