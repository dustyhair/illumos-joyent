/* Linux i386 raw ABI. No libc, interpreter, headers, or runtime. */
.section .data
pid: .long 0
tid: .long 0
mask: .long 0, 8 /* signal 36, bit 35 */
zero: .long 0, 0
/* Linux i386 siginfo union starts at 12, total size 128. */
info: .long 10, 7, -1, 12345, 23456, 0xfedcba98
.zero 104
got: .zero 128
pass: .ascii "PASS queued_signal32 layout=128 union=12 value=fedcba98\n"
.set plen, .-pass
failmsg: .ascii "FAIL queued_signal32 exit=status-stage\n"
.set flen, .-failmsg
.section .text
.global _start
.macro check n, want
 mov $\n, %ebp
 cmp $\want, %eax
 jne fail
.endm
/* Inputs ebx=tgid ecx=tid edx=signal esi=info. */
queue:
 mov $335, %eax
 int $0x80
 ret
self:
 mov pid, %ebx
 mov tid, %ecx
 mov $info, %esi
 ret
waitone:
 mov $177, %eax
 mov $mask, %ebx
 mov $got, %ecx
 mov $zero, %edx
 mov $8, %esi
 int $0x80
 ret
_start:
 mov $20, %eax
 int $0x80
 mov %eax, pid
 mov $224, %eax
 int $0x80
 mov %eax, tid
 mov $175, %eax
 xor %ebx, %ebx /* SIG_BLOCK */
 mov $mask, %ecx
 xor %edx, %edx
 mov $8, %esi
 int $0x80
 check 1, 0
 call self
 mov $36, %edx
 call queue
 check 2, 0
 call waitone
 check 3, 36
 mov got, %eax
 check 4, 36
 mov got+4, %eax
 check 5, 7
 mov got+8, %eax
 check 6, -1
 mov got+12, %eax
 check 7, 12345
 mov got+16, %eax
 check 8, 23456
 mov got+20, %eax
 check 9, 0xfedcba98
 call waitone
 check 10, -11
 call self
 xor %edx, %edx
 call queue
 check 11, 0
 call waitone
 check 12, -11
 call self
 xor %edx, %edx
 xor %ebx, %ebx
 call queue
 check 13, -22
 call self
 mov $-1, %ecx
 call queue
 check 14, -22
 call self
 mov $0x7fffffff, %ecx
 mov $65, %edx
 call queue
 check 15, -3
 call self
 mov $0x7fffffff, %ebx
 xor %edx, %edx
 call queue
 check 16, -3
 call self
 mov $65, %edx
 call queue
 check 17, -22
 call self
 xor %edx, %edx
 movl $0, info+8
 call queue
 check 18, 0
 movl $-6, info+8
 call queue
 check 19, 0
 movl $1, info+8
 call queue
 check 20, 0
 mov $0x7fffffff, %ecx
 call queue
 check 21, -1
 movl $0, info+8
 call queue
 check 22, -1
 movl $-6, info+8
 call queue
 check 23, -1
 call self
 xor %esi, %esi
 call queue
 check 24, -14
 call waitone
 check 25, -11
 mov $4, %eax
 mov $1, %ebx
 mov $pass, %ecx
 mov $plen, %edx
 int $0x80
 xor %ebx, %ebx
 jmp exit
fail:
 mov $4, %eax
 mov $2, %ebx
 mov $failmsg, %ecx
 mov $flen, %edx
 int $0x80
 mov %ebp, %ebx
exit:
 mov $1, %eax
 int $0x80
.section .note.GNU-stack,"",@progbits
