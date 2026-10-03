/*
 * Verify that branches do not rewrite operating-system mapping counts.
 * macOS AArch64 host test; build with run_aarch64_codegen_tests.sh.
 * Only the MMU calls and diagnostic sink are replaced by test doubles.
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sys/mman.h>

#include "cpu/cpu_jitc_aarch64/jitc.h"
#include "cpu/cpu_jitc_aarch64/ppc_mmu.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error This native execution test requires macOS AArch64.
#endif

static PPC_CPU_State cpu;
extern "C" {
PPC_CPU_State *gCPU = &cpu;
}

void ppc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void jitcDebugLogEmit(JITC &, const byte *, int) {}

#include "cpu/cpu_jitc_aarch64/ppc_opc.h"

static unsigned safeguardCalls;
// Sentinel for the removed OS-specific branch hook. Linking the old generator
// to this test makes the regression observable without any MMU dependencies.
extern "C" void ppc_safeguard_map_drain_busy(PPC_CPU_State *state)
{
    safeguardCalls++;
    state->gpr[4] = 1;
}

extern "C" void runBranch(PPC_CPU_State *state, NativeAddress entry);
__asm__(
    ".text\n.p2align 2\n_runBranch:\n"
    "stp x19, x20, [sp, #-32]!\n"
    "str x30, [sp, #16]\n"
    "mov x20, x0\n"
    "adr x19, 1f\n"
    "br x1\n"
    "1:\nldr x30, [sp, #16]\n"
    "ldp x19, x20, [sp], #32\nret\n");

int main()
{
    constexpr size_t size = 4 * 1024 * 1024;
    byte *cache = static_cast<byte *>(mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                                         MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
    if (cache == MAP_FAILED) return 1;
    // lwz; rlwinm; cmplwi; beqlr++; b -16, as in XNU mapDrainBusy.
    uint32 code[1024] = {};
    uint32 words[] = {0x80830000, 0x5484463e, 0x28040001, 0x4de20020, 0x4bfffff0};
    for (unsigned i = 0; i < 5; i++) code[i] = __builtin_bswap32(words[i]);
    unsigned checks = 0;
    for (int backwards = 0; backwards < 2; backwards++) {
        for (unsigned left = 4; left <= FRAGMENT_SIZE; left += 4) {
            pthread_jit_write_protect_np(0);
            JITC jitc = {};
            ClientPage page = {};
            TranslationCacheFragment first = {cache + backwards * 2 * 1024 * 1024, nullptr};
            TranslationCacheFragment next = {cache + (1 - backwards) * 2 * 1024 * 1024, nullptr};
            // Replace the loop target with a return to the host test wrapper.
            NativeAddress target = cache + 4096;
            uint32 ret = a64_BR(X19);
            memcpy(target, &ret, sizeof(ret));
            page.entrypoints[0] = target;
            page.tcf_current = &first;
            page.bytesLeft = left;
            page.tcp = first.base + FRAGMENT_SIZE - left;
            jitc.translationCache = cache;
            jitc.currentPage = &page;
            jitc.currentPhysPage = reinterpret_cast<byte *>(code);
            jitc.freeFragmentsList = &next;
            jitc.nativeFlags = PPC_NO_CRx;
            jitc.pc = 16;
            jitc.current_opc = words[4];
            NativeAddress entry = page.tcp;
            ppc_opc_gen_bx(jitc);
            __builtin___clear_cache(reinterpret_cast<char *>(cache), reinterpret_cast<char *>(cache + size));
            pthread_jit_write_protect_np(1);
            for (uint32 busy : {0u, 1u, 2u, 3u, 255u}) {
                memset(&cpu, 0, sizeof(cpu));
                cpu.gpr[3] = 0x049fbac0;
                cpu.gpr[4] = busy;
                cpu.cr = 0x44820022;
                runBranch(&cpu, entry);
                if (safeguardCalls || cpu.gpr[3] != 0x049fbac0 || cpu.gpr[4] != busy || cpu.cr != 0x44820022) {
                    ppc_fatal("FAIL: branch changed guest state (busy=%u, calls=%u)\n", busy, safeguardCalls);
                }
                checks++;
            }
        }
    }
    munmap(cache, size);
    printf("PASS: %u branch executions preserve guest mapping state\n", checks);
}
