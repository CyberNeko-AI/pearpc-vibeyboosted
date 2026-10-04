// Execute the real fallback emitter and exception handler at fragment edges.
// The MMU invalidation and NEW_PC destination are test doubles; the exception
// marker, saved CPU state and native conditional dispatch are real.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <pthread.h>
#include <sys/mman.h>
#include "cpu/cpu_jitc_aarch64/ppc_opc.h"
#include "cpu/cpu_jitc_aarch64/ppc_exc.h"
#include "cpu/cpu_jitc_aarch64/ppc_mmu.h"

static PPC_CPU_State cpu;
extern "C" { PPC_CPU_State *gCPU = &cpu; }
static unsigned invalidations;
void ppc_mmu_tlb_invalidate(PPC_CPU_State &) { ++invalidations; }
void jitcDebugLogEmit(JITC &, const byte *, int) {}
void ppc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}
void ppc_set_singlestep_v(bool, const char *, int, const char *, ...) { abort(); }
static int helper(PPC_CPU_State &state)
{
    if (state.gpr[0]) {
        ppc_exception(state, PPC_EXC_PROGRAM, PPC_EXC_PROGRAM_PRIV);
    } else {
        state.gpr[9] = 42;
    }
    return 0; // Existing non-memory interpreters also return 0 after a fault.
}
extern "C" void runFallback(PPC_CPU_State *state, NativeAddress entry);
__asm__(
    ".text\n.p2align 2\n_runFallback:\n"
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
    unsigned checks = 0;
    for (int backwards = 0; backwards < 2; ++backwards) {
        for (unsigned left = 4; left <= FRAGMENT_SIZE; left += 4) {
            for (uint32 pc : {0x0f9232bcu, 0x000006fcu}) {
                pthread_jit_write_protect_np(0);
                JITC jitc = {};
                ClientPage page = {};
                TranslationCacheFragment first = {cache + backwards * 2 * 1024 * 1024, nullptr};
                TranslationCacheFragment next = {cache + (1 - backwards) * 2 * 1024 * 1024, nullptr};
                page.tcf_current = &first;
                page.bytesLeft = left;
                page.tcp = first.base + FRAGMENT_SIZE - left;
                jitc.translationCache = cache;
                jitc.currentPage = &page;
                jitc.freeFragmentsList = &next;
                jitc.nativeFlags = PPC_NO_CRx;
                jitc.pc = pc & 0xfff;
                jitc.current_opc = 0x7d3f42a6;
                NativeAddress entry = page.tcp;
                ppc_opc_gen_interpret(jitc, helper);
                // Mark erroneous fallthrough without accessing any MMIO.
                jitc.asmMOV(W0, 1);
                jitc.asmSTRw_cpu(W0, offsetof(PPC_CPU_State, gpr[10]));
                jitc.emit32(a64_BR(X19));
                uint32 dispatch[] = {
                    a64_STRw(W0, X20, offsetof(PPC_CPU_State, gpr[11])),
                    a64_BR(X19)
                };
                memcpy(cache + 8192, dispatch, sizeof dispatch);
                __builtin___clear_cache(reinterpret_cast<char *>(cache), reinterpret_cast<char *>(cache + size));
                pthread_jit_write_protect_np(1);
                for (bool fault : {false, true}) {
                    memset(&cpu, 0, sizeof cpu);
                    cpu.current_code_base = pc & ~0xfff;
                    cpu.msr = MSR_PR | MSR_IR | MSR_DR;
                    cpu.have_reservation = true;
                    cpu.interpreter_exception = true; // stale marker must be cleared
                    cpu.gpr[0] = fault;
                    cpu.gpr[9] = 0x1234;
                    cpu.stubs[PPC_STUB_NEW_PC] = cache + 8192;
                    invalidations = 0;
                    runFallback(&cpu, entry);
                    bool okay = cpu.pc == pc && cpu.current_opc == 0x7d3f42a6;
                    if (fault) {
                        okay &= cpu.interpreter_exception && cpu.gpr[10] == 0 && cpu.gpr[11] == PPC_EXC_PROGRAM &&
                                cpu.npc == PPC_EXC_PROGRAM && cpu.srr[0] == pc &&
                                cpu.srr[1] == (MSR_PR | MSR_IR | MSR_DR | PPC_EXC_PROGRAM_PRIV) &&
                                cpu.gpr[9] == 0x1234 && cpu.msr == 0 && !cpu.have_reservation && invalidations == 1;
                    } else {
                        okay &= !cpu.interpreter_exception && cpu.gpr[10] == 1 && cpu.gpr[11] == 0 &&
                                cpu.npc == pc + 4 && cpu.gpr[9] == 42 && invalidations == 0 &&
                                cpu.msr == (MSR_PR | MSR_IR | MSR_DR) && cpu.have_reservation;
                    }
                    if (!okay) ppc_fatal("FAIL fallback pc=%08x left=%u backwards=%d fault=%d\n", pc, left, backwards, fault);
                    ++checks;
                }
            }
        }
    }
    munmap(cache, size);
    printf("PASS: %u fallback exception/normal executions across fragment boundaries\n", checks);
}
