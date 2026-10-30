/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
.section .data
header: .long 0, 0
wanted: .zero 24
actual: .zero 24
saved: .zero 24
stage: .long 1
.section .text
.global _start
get:
    mov $184, %eax
    mov $header, %ebx
    int $0x80
    ret
set:
    mov $185, %eax
    mov $header, %ebx
    mov $wanted, %ecx
    int $0x80
    ret
compare:
    mov $wanted, %esi
    mov $actual, %edi
    mov $6, %ecx
    repe cmpsl
    jne fail
    ret
_start:
    cld
    xor %ecx, %ecx
    call get
    test %eax, %eax
    jne fail
    cmpl $0x20080522, header
    jne fail
    incl stage
    movl $-1, header+4
    xor %ecx, %ecx
    call get
    test %eax, %eax
    jne fail
    mov $wanted, %ecx
    call get
    cmp $-22, %eax
    jne fail
    incl stage
    movl $0, header+4
    mov $wanted, %ecx
    call get
    test %eax, %eax
    jne fail
    testl $0x400, wanted+4
    jz fail
    incl stage
    orl $0x400, wanted+8
    call set
    test %eax, %eax
    jne fail
    mov $actual, %ecx
    call get
    test %eax, %eax
    jne fail
    call compare
    incl stage
    andl $~0x400, wanted+8
    call set
    test %eax, %eax
    jne fail
    mov $actual, %ecx
    call get
    test %eax, %eax
    jne fail
    call compare
    incl stage
    mov $wanted, %esi
    mov $saved, %edi
    mov $6, %ecx
    rep movsl
    andl $~0x400, wanted+4
    call set
    cmp $-1, %eax
    jne fail
    mov $actual, %ecx
    call get
    test %eax, %eax
    jne fail
    mov $saved, %esi
    mov $wanted, %edi
    mov $6, %ecx
    rep movsl
    call compare
    incl stage
    mov $wanted, %edi
    xor %eax, %eax
    mov $6, %ecx
    rep stosl
    call set
    test %eax, %eax
    jne fail
    mov $actual, %ecx
    call get
    test %eax, %eax
    jne fail
    call compare
    xor %ebx, %ebx
    jmp done
fail:
    mov stage, %ebx
done:
    mov $1, %eax
    int $0x80
.section .note.GNU-stack,"",@progbits
