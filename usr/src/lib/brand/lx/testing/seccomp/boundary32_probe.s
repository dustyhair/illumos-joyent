/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
.section .text
.global _start
_start:
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    movl $251, %eax
    int $0x80
    cmpl $-38, %eax
    jne failed
    movl $359, %eax
    int $0x80
    cmpl $-38, %eax
    jne failed
    movl $360, %eax
    int $0x80
    cmpl $-38, %eax
    jne failed
    movl $252, %eax
    xorl %ebx, %ebx
    int $0x80
failed:
    movl $252, %eax
    movl $1, %ebx
    int $0x80
.section .note.GNU-stack,"",@progbits
