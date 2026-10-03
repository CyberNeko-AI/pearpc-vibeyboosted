/* Changing IBAT1L must redispatch the NEXT instruction into its new page. */
    .text
    .globl _start
_start:
    lis 10,0x40
    lis 4,0x40
    ori 4,4,2
    mtspr 537,4
    mtspr 536,4
    isync
    /* Page A: mtspr IBAT1L,r7; li r3,1; test_exit */
    lis 5,0x7cf3
    ori 5,5,0x83a6
    stw 5,0(10)
    lis 5,0x3860
    ori 5,5,1
    stw 5,4(10)
    lis 6,0x33
    ori 6,6,0x3304
    stw 6,8(10)
    /* Page B lives 128 KiB later; the same next EA now exits successfully. */
    lis 7,0x42
    ori 7,7,2
    mtspr 537,7
    isync
    lis 5,0x6000
    stw 5,0(10)
    lis 5,0x3860
    stw 5,4(10)
    stw 6,8(10)
    sync
    mtspr 531,4
    lis 8,0x100
    ori 8,8,2
    mtspr 530,8
    isync
    lis 9,0x100
    mtctr 9
    bctr
    .data
    .long 0
