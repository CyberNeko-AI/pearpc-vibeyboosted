/* A zero-bit whole-vector shift must copy all 128 bits, including aliases. */
    .macro check_vector vr
    lis 8,0x20
    addi 9,8,16
    stvx \vr,0,9
    li 11,4
    mtctr 11
1:
    lwz 3,0(8)
    lwz 4,0(9)
    cmpw 3,4
    beq 2f
    addi 30,30,1
2:
    addi 8,8,4
    addi 9,9,4
    bdnz 1b
    .endm
    .text
    .globl _start
_start:
    mfmsr 4
    oris 4,4,0x0200
    mtspr 27,4
    lis 3,0x10
    ori 3,3,0x20
    mtspr 26,3
    rfi
    nop
    /* Entry above deliberately uses 0x100020, avoiding absolute relocations. */
    li 30,0
    lis 8,0x20
    lvx 1,0,8
    vxor 2,2,2
    vsl 3,1,2
    check_vector 3
    vsr 4,1,2
    check_vector 4
    vsl 1,1,2
    check_vector 1
    lis 8,0x20
    lvx 1,0,8
    vsr 2,1,2
    check_vector 2
    mr 3,30
    .long 0x00333304
    .data
    .long 0x01234567,0x89abcdef,0x10203040,0x50607080
    .space 16
