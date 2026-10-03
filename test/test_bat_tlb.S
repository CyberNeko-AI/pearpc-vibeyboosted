/* DBAT changes must invalidate cached data mappings. No disks or OS required.
 * Failure count is returned through the PearPC bare-metal exit opcode.
 * Data at EA 0x200000 has a PROM-created PTE distinct from PA 0x200000.
 */
    .text
    .globl _start
_start:
    li 30,0
    lis 10,0x20
    lis 11,0x1122
    ori 11,11,0x3344
    lis 12,0xaabb
    ori 12,12,0xccdd
    li 0,0

    /* Prime a temporary identity DBAT, then revoke it. */
    lis 4,0x20
    ori 4,4,2
    mtspr 537,4
    mtspr 536,4
    isync
    stw 12,0(10)
    lwz 5,0(10)
    mtspr 536,0
    isync
    lwz 5,0(10)
    cmpw 5,11
    beq 1f
    addi 30,30,1
1:
    /* A stale write TLB must not send this store to the old identity PA. */
    stw 11,0(10)
    tlbie 10
    sync
    /* Read the physical identity page again: it must retain aabbccdd. */
    mtspr 536,4
    isync
    lwz 5,0(10)
    cmpw 5,12
    beq 2f
    addi 30,30,1
2:
    stw 12,0(10)
    mtspr 536,0
    tlbie 10
    stw 11,0(10)
    lwz 5,0(10)

    /* Installing a BAT must supersede an already cached PTE. */
    mtspr 536,4
    isync
    stw 12,0(10)
    mtspr 536,0
    tlbie 10
    lwz 5,0(10)
    cmpw 5,11
    beq 3f
    addi 30,30,1
3:
    stw 11,0(10)

    /* Retarget DBAT0L without changing DBAT0U or MSR. */
    lis 6,0x40
    ori 6,6,2
    mtspr 537,6
    mtspr 536,4
    isync
    stw 11,0(10)
    lis 7,0x60
    ori 7,7,2
    mtspr 537,7
    isync
    stw 12,0(10)
    tlbie 10
    lwz 5,0(10)
    cmpw 5,12
    beq 4f
    addi 30,30,1
4:
    mtspr 537,6
    tlbie 10
    lwz 5,0(10)
    cmpw 5,11
    beq 5f
    addi 30,30,1
5:
    /* A larger block changes the BRPN mask as well as BEPI/BL. */
    mtspr 537,4
    tlbie 10
    stw 12,0(10)          /* PA 0x200000 = aabbccdd */
    mtspr 537,7
    tlbie 10
    stw 11,0(10)          /* PA 0x600000 = 11223344 */
    li 8,0x1fe           /* 16 MiB BAT block, Vs=1, BEPI=0 */
    mtspr 536,8
    isync
    tlbie 10             /* Even a miss must use the recomputed BRPN. */
    lwz 5,0(10)          /* Correct PA=0x200000, stale BRPN gives 0x600000 */
    cmpw 5,12
    beq 6f
    addi 30,30,1
6:
    mtspr 536,0
    tlbie 10
    mr 3,30
    .long 0x00333304
    .data
    .long 0x11223344
