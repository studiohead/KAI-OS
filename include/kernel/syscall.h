// include/kernel/syscall.h
// KAI OS: Safe syscall interface for EL0 AI sandbox
// Declares syscalls and helper types for kernel <-> AI communication.

#ifndef KAI_SYSCALL_H
#define KAI_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Capability Flags
// Each AI session or caller has a capabilities mask that restricts which
// syscalls it can perform. All syscalls should validate the caller's caps.
// -----------------------------------------------------------------------------
#define CAP_NONE      0          /* No capabilities */
#define CAP_READ_MEM  (1 << 0)   /* Allows reading memory info or arbitrary memory (sys_mem_read) */
#define CAP_WRITE_MEM (1 << 1)   /* Allows writing memory */
#define CAP_MMIO      (1 << 2)   /* Allows MMIO register access */
#define CAP_SYSTEM    (1 << 3)   /* Allows privileged system operations */

// -----------------------------------------------------------------------------
// Syscall: UART Write
// Safely writes 'len' bytes from 'buf' to UART.
// Parameters:
//   - buf         : Pointer to byte buffer to send
//   - len         : Number of bytes to write
//   - caller_caps : Bitmask of caller capabilities
// Returns:
//   - >=0 : Number of bytes successfully written
//   -  -1 : Failure (e.g., caller lacks CAP_MMIO)
// -----------------------------------------------------------------------------
int sys_uart_write(const char *buf, size_t len, uint32_t caller_caps);

// -----------------------------------------------------------------------------
// Syscall: UART Put String
// Writes a null-terminated string to UART.
//
// This is a convenience wrapper around sys_uart_write that automatically
// calculates the string length by scanning until the null terminator.
//
// Parameters:
//   - str         : Pointer to null-terminated string
//   - caller_caps : Bitmask of caller capabilities (requires CAP_MMIO)
//
// Returns:
//   - >=0 : Number of bytes successfully written (excluding null terminator)
//   -  -1 : Failure (invalid caps or NULL pointer)
// -----------------------------------------------------------------------------
int sys_uart_puts(const char *str, uint32_t caller_caps);


// -----------------------------------------------------------------------------
// Syscall: UART Put Character
// Writes a single character to UART.
//
// This is a convenience wrapper around sys_uart_write for single-byte output.
//
// Parameters:
//   - c           : Character to write
//   - caller_caps : Bitmask of caller capabilities (requires CAP_MMIO)
//
// Returns:
//   - 1  : Success
//   - -1 : Failure (caller lacks CAP_MMIO)
// -----------------------------------------------------------------------------
int sys_uart_putc(char c, uint32_t caller_caps);

// -----------------------------------------------------------------------------
// Syscall: UART Hex 64-bit
// Writes a 64-bit value as exactly 16 uppercase hex digits to UART.
// Parameters:
//   - value       : 64-bit value to display
//   - caller_caps : Bitmask of caller capabilities (requires CAP_MMIO)
// Returns:
//   - 16 : Success (always 16 hex digits written)
//   - -1 : Failure (caller lacks CAP_MMIO)
// -----------------------------------------------------------------------------
int sys_uart_hex64(uint64_t value, uint32_t caller_caps);

// -----------------------------------------------------------------------------
// Syscall: Memory Read
// Safely reads 'len' bytes from source address into dst_buf.
// Performs capability checks to prevent unauthorized access.
// Parameters:
//   - src_addr    : Source memory address to read from
//   - dst_buf     : Destination buffer
//   - len         : Number of bytes to read
//   - caller_caps : Bitmask of caller capabilities
// Returns:
//   - 0  : Success
//   - -1 : Failure (invalid caps or NULL pointers)
// -----------------------------------------------------------------------------
int sys_mem_read(uintptr_t src_addr, void *dst_buf, size_t len, uint32_t caller_caps);

// -----------------------------------------------------------------------------
// Syscall: Memory Info
// Provides safe inspection of kernel memory layout for BSS and stack.
// Capability check ensures only authorized AI sessions can query.
// Parameters:
//   - bss_start   : Receives start address of BSS section
//   - bss_end     : Receives end address of BSS section
//   - stack_start : Receives bottom of stack
//   - stack_end   : Receives top of stack
//   - caller_caps : Bitmask of caller capabilities
// Returns:
//   - 0  : Success
//   - -1 : Failure (unauthorized or NULL pointers)
// -----------------------------------------------------------------------------
int sys_mem_info(uintptr_t *bss_start, uintptr_t *bss_end,
                 uintptr_t *stack_start, uintptr_t *stack_end,
                 uint32_t caller_caps);

#ifdef __cplusplus
}
#endif

#endif // KAI_SYSCALL_H