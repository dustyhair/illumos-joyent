/* CDDL version 1.0, http://www.illumos.org/license/CDDL. */
.section .text
.global _start
_start:
    movl $1, %ebp
    movl $172, %eax
    movl $38, %ebx
    movl $1, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    int $0x80
    testl %eax, %eax
    jne failed
    movl $172, %eax
    movl $22, %ebx
    movl $2, %ecx
    movl $program, %edx
    int $0x80
    testl %eax, %eax
    jne failed

    /* All three supported clocks accept the 8-byte relative request. */
    incl %ebp
    movl $265, %eax
    movl $1, %ebx
    movl $before, %ecx
    int $0x80
    testl %eax, %eax
    jne failed
    xorl %ebx, %ebx
    call relative
    movl $1, %ebx
    call relative
    movl $7, %ebx
    call relative
    movl $265, %eax
    movl $1, %ebx
    movl $after, %ecx
    int $0x80
    testl %eax, %eax
    jne failed
    movl after, %eax
    subl before, %eax
    js failed
    cmpl $1, %eax
    jg elapsed
    movl after+4, %edx
    subl before+4, %edx
    testl %eax, %eax
    jz same_second
    addl $1000000000, %edx
same_second:
    cmpl $3000000, %edx
    jl failed
elapsed:
    /* Success must not touch either word or the adjacent canary. */
    cmpl $123, remaining
    jne failed
    cmpl $456, remaining+4
    jne failed
    cmpl $0x13579bdf, remaining+8
    jne failed

    incl %ebp
    xorl %ebx, %ebx
    call absolute
    movl $1, %ebx
    call absolute
    movl $7, %ebx
    call absolute

    incl %ebp
    movl $267, %eax
    movl $255, %ebx
    xorl %ecx, %ecx
    movl $1, %edx
    int $0x80
    cmpl $-22, %eax
    jne failed
    movl $267, %eax
    movl $4, %ebx
    int $0x80
    cmpl $-95, %eax
    jne failed
    movl $267, %eax
    movl $1, %ebx
    int $0x80
    cmpl $-14, %eax
    jne failed
    movl $267, %eax
    movl $bad_request, %edx
    int $0x80
    cmpl $-22, %eax
    jne failed

    incl %ebp
    movl $267, %eax
    movl $2, %ecx
    movl $past, %edx
    movl $1, %esi
    int $0x80
    testl %eax, %eax
    jne failed
    movl $4, %eax
    movl $1, %ebx
    movl $message, %ecx
    movl $message_end-message, %edx
    int $0x80
    movl $252, %eax
    xorl %ebx, %ebx
    int $0x80
relative:
    movl $267, %eax
    xorl %ecx, %ecx
    movl $request, %edx
    movl $remaining, %esi
    int $0x80
    testl %eax, %eax
    jne failed
    ret
absolute:
    movl $267, %eax
    movl $1, %ecx
    movl $past, %edx
    movl $1, %esi
    int $0x80
    testl %eax, %eax
    jne failed
    ret
failed:
    movl $252, %eax
    movl %ebp, %ebx
    int $0x80
.section .data
.balign 4
program:
    .short 1
    .short 0
    .long allow
allow:
    .short 6
    .byte 0,0
    .long 0x7fff0000
request: .long 0,1000000
past: .long 0,0
bad_request: .long 0,1000000000
remaining: .long 123,456,0x13579bdf
before: .long 0,0
after: .long 0,0
message: .ascii "CLOCK_SLEEP32_PASS\n"
message_end:
.section .note.GNU-stack,"",@progbits
