/* User-mode SPR faults must dispatch before the next translated instruction.
 * A guard store reproduces the secondary machine check seen by Mandrake.
 */
    .text
    .globl _start
_start:
    /* Install a position-independent Program exception handler at PA 0x700. */
    lis 5, handler@ha
    addi 5, 5, handler@l
    li 6, 0x700
    li 7, (handler_end-handler)/4
    mtctr 7
1:
    lwz 0, 0(5)
    stw 0, 0(6)
    addi 5, 5, 4
    addi 6, 6, 4
    bdnz 1b
    li 6, 0x700
    icbi 0, 6
    sync
    isync
    li 30, 0
    li 31, -4

.macro PROBE instruction
    lis 25, 2f@ha
    addi 25, 25, 2f@l
    mtsrr0 25
    li 4, 0x4030 /* MSR[PR,IR,DR]; keep the PROM code mappings. */
    mtsrr1 4
    li 9, 0x1234
    rfi
2:
    \instruction
    stw 9, 0(31) /* Must be skipped by the exception handler. */
    cmpw 26, 25
    bne fail
    lis 4, 0x4
    ori 4, 4, 0x4030
    cmpw 27, 4 /* Saved PR plus privilege violation, exact SRR0 above. */
    bne fail
    cmpwi 9, 0x1234 /* Faulting read must not change its destination. */
    bne fail
.endm
    PROBE "mfspr 9,287"  /* PVR: exact opcode 0x7d3f42a6 from the crash. */
    PROBE "mfspr 9,25"   /* SDR1 */
    PROBE "mfspr 9,22"   /* DEC */
    PROBE "mfspr 9,1008" /* HID0 */
    PROBE "mtspr 22,9"   /* DEC write */
    PROBE "mtspr 1008,9" /* HID0 write */
    cmpwi 30, 6
    bne fail
    /* Successful user SPR operations must still fall through. */
    lis 4, allowed@ha
    addi 4, 4, allowed@l
    mtsrr0 4
    li 4, 0x4030
    mtsrr1 4
    rfi
allowed:
    li 9, 0
    mtspr 1, 9
    mfspr 9, 1
    cmpwi 9, 0
    bne fail
    li 9, 42
    mtspr 256, 9
    li 9, 0
    mfspr 9, 256
    cmpwi 9, 42
    bne fail
    li 3, 0
    .long 0x00333304
fail:
    li 3, 1
    .long 0x00333304
handler:
    mfsrr0 26
    mfsrr1 27
    addi 30, 30, 1
    addi 28, 26, 8
    mtsrr0 28
    li 28, 0x30
    mtsrr1 28
    rfi
handler_end:
    .data
    .long 0
