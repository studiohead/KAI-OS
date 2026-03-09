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
#   all       — build ELF + raw binary image (default)
#   run       — launch QEMU in the current terminal (portable)
#   clean     — remove all build artefacts
#   size      — print section sizes
# =============================================================================

# ---- Toolchain -------------------------------------------------------------
# These can be overridden from the command line, e.g., make CC=gcc
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
# Architecture-specific assembly files
ASM_SRCS := $(SRC_DIR)/arch/aarch64/boot.S \
             $(SRC_DIR)/arch/aarch64/mmu.S

# Core kernel and subsystem C files
# sandbox_isr.c has been removed as its logic is now consolidated in irq.c
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
IMG := $(BUILD_DIR)/kernel.img   # flat binary for real hardware / inspection

# ---- QEMU ------------------------------------------------------------------
# -M virt: Generic ARM Virt machine
# -cpu max: Support all available features (VHE, PAC, etc.)
# -nographic: Redirect serial to current terminal
QEMU       := qemu-system-aarch64
QEMU_FLAGS := \
    -M virt                 \
    -cpu max                \
    -nographic              \
    -kernel $(ELF)

# ============================================================================
.PHONY: all run clean size

# Default target
all: $(ELF) $(IMG)

# ---- Link ------------------------------------------------------------------
# Combines all objects using the custom linker script
$(ELF): $(ALL_OBJS)
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $^ -o $@

# ---- Flat binary -----------------------------------------------------------
# Useful for analyzing the layout or flashing to physical media
$(IMG): $(ELF)
	$(OBJCOPY) -O binary $< $@

# ---- Compile C sources -----------------------------------------------------
# Uses patsubst to preserve the directory hierarchy inside /build
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Assemble ASM sources --------------------------------------------------
# Both .S and .c files use the same compiler front-end for convenience
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Run in current terminal (works on macOS, Linux, WSL) -----------------
# This target builds and then immediately launches the kernel
run: all
	$(QEMU) $(QEMU_FLAGS)

# ---- Section size report ---------------------------------------------------
# Helps monitor the size of .text, .data, and .bss
size: $(ELF)
	$(SIZE) $<

# ---- Clean -----------------------------------------------------------------
# Removes the build directory completely
clean:
	rm -rf $(BUILD_DIR)