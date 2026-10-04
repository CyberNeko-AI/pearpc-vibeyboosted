// Independent byte-stream oracle for AltiVec whole-vector shifts. Exercise
// the real interpreter handlers used by each backend, including aliases.
#include <cstdio>
#include <cstring>
#include <cstdint>
#ifdef TEST_GENERIC
#include "cpu/cpu_generic/ppc_cpu.h"
#include "cpu/cpu_generic/ppc_vec.h"
PPC_CPU_State gCPU;
#define STATE gCPU
#define EXEC(name) ppc_opc_##name()
#else
#include "cpu/cpu_jitc_aarch64/ppc_cpu.h"
#include "cpu/cpu_jitc_aarch64/ppc_vec.h"
static PPC_CPU_State cpu;
#define STATE cpu
#define EXEC(name) ppc_opc_##name(STATE)
#endif
int main()
{
    unsigned checks = 0;
    uint32_t seed = 0xc0ffee;
    for (unsigned trial = 0; trial < 256; ++trial) {
        unsigned char input[16];
        for (unsigned i = 0; i < 16; ++i) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            input[i] = seed;
        }
        for (unsigned shift = 0; shift < 8; ++shift) {
            for (unsigned direction = 0; direction < 2; ++direction) {
                for (unsigned dest = 0; dest < 3; ++dest) {
                    for (unsigned i = 0; i < 16; ++i) {
                        VECT_B(STATE.vr[1], i) = input[i];
                        VECT_B(STATE.vr[2], i) = ((i * 13) & 0xf8) | shift;
                    }
                    STATE.current_opc = (4u << 26) | (dest << 21) | (1 << 16) | (2 << 11) |
                                        (direction ? 0x2c4 : 0x1c4);
                    STATE.cr = 0x12345678;
                    STATE.vscr = 0x10000;
                    if (direction) { EXEC(vsr); } else { EXEC(vsl); }
                    for (unsigned i = 0; i < 16; ++i) {
                        unsigned expected = input[i];
                        if (shift) {
                            if (!direction) {
                                expected = (input[i] << shift) |
                                    (i < 15 ? input[i + 1] >> (8 - shift) : 0);
                            } else {
                                expected = (input[i] >> shift) |
                                    (i > 0 ? input[i - 1] << (8 - shift) : 0);
                            }
                        }
                        unsigned actual = VECT_B(STATE.vr[dest], i);
                        if (actual != (expected & 255)) {
                            fprintf(stderr, "FAIL %s shift=%u dest=%u byte=%u: %02x != %02x\n",
                                    direction ? "vsr" : "vsl", shift, dest, i, actual, expected & 255);
                            return 1;
                        }
                    }
                    if (STATE.cr != 0x12345678 || STATE.vscr != 0x10000) return 2;
                    ++checks;
                }
            }
        }
    }
    printf("PASS: %u whole-vector shift cases\n", checks);
}
