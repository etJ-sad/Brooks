/* SPDX-License-Identifier: GPL-2.0-only */
/* void enter_kernel(cr3 = rcx, gdtr = rdx, stack = r8, entry = r9)  (MS ABI)
 * Runs from the identity-mapped loader image: switch page tables and stack,
 * load our GDT (before CR3, the GDTR itself is not identity-mapped), reload segments and jump to the kernel with IF = 0. */
.text
.globl enter_kernel
enter_kernel:
    cli
    lgdt (%rdx)          /* gdtr is physical (old tables); its base is HHDM */
    mov %rcx, %cr3
    mov %r8, %rsp
    pushq $0x28
    lea 1f(%rip), %rax
    pushq %rax
    lretq
1:
    mov $0x30, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %eax, %fs
    mov %eax, %gs
    xor %ebp, %ebp
    pushq $0
    jmp *%r9
