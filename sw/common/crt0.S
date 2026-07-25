.section .text.init
.globl _start
_start:
    la   sp, __stack_top

    .option push
    .option norelax
    la   gp, __global_pointer$
    .option pop

    la   t0, __bss_start
    la   t1, __bss_end
1:  bgeu t0, t1, 2f
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    1b
2:
    call main

.globl _exit
_exit:
    slli a0, a0, 1
    ori  a0, a0, 1
    la   t0, tohost
    sw   a0, 0(t0)
1:  j    1b

.section .tohost, "aw", @progbits
.align 6
.globl tohost
tohost:   .dword 0
.globl fromhost
fromhost: .dword 0