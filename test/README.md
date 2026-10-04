# PearPC Test ELFs

Minimal PPC ELF test programs for validating the JIT compiler.

## How tests work

Each test is a bare-metal PPC assembly program (`.S` file) that runs
directly on the emulated CPU. No OS, no libc. The PROM's ELF loader
(`mapped_load_elf()` in `src/io/prom/promboot.cc`) loads the ELF into
memory with 1:1 page table mappings and jumps to `_start`.

Tests use custom opcodes for I/O:
- `.long 0x00333303` — print string (`r3` = address, `r4` = length)
- `.long 0x00333304` — exit (`r3` = exit code, 0 = success)

These are handled in `ppc_opc_special()` in `ppc_dec.cc`.

## Memory layout

- Code (`.text`) loaded at `0x100000` (defined by `test_loop.ld` linker script)
- Data (`.data`/`.bss`) at `0x200000` — must be on a separate page from code to avoid JIT self-modifying code issues
- Stack set up by the ELF loader (mapped pages near the code)
- Page table at PA `0x300000` (set up by the PROM, SDR1 = `0x300003`)
- MMU is ON (MSR = `0x2030`: IR=1, DR=1)

**Important:** Do not use addresses within the `.text` section for scratch data — it overwrites your own instructions. Use `.data` section labels instead.

## Building

Tests are cross-compiled using a PPC cross-compiler in Docker (amd64):

```sh
docker run --rm --platform linux/amd64 \
    -v $(pwd)/test:/work -w /work \
    debian:bookworm bash -c '
    apt-get update -qq >/dev/null 2>&1 &&
    apt-get install -y -qq gcc-powerpc-linux-gnu binutils-powerpc-linux-gnu >/dev/null 2>&1 &&
    powerpc-linux-gnu-as -mbig -mregnames -o TEST.o TEST.S &&
    powerpc-linux-gnu-ld -T test_loop.ld -o TEST.elf TEST.o
'
```

Or use the helper script for `test_loop`:
```sh
./test/build_ppc_elf.sh
```

## Running

Run all tests:
```sh
make test
# or directly:
test/run_tests.sh
```

Each test is run with a 30-second timeout. If a test hangs (e.g. infinite
loop due to a JIT bug), it is killed and reported as `TIMEOUT`. You can
override the timeout:
```sh
test/run_tests.sh ./src/ppc 60   # 60-second timeout per test
```

Run a single test:
```sh
# Headless:
./src/ppc --headless test/test_alu.cfg

# With GUI (use for boot tests):
./src/ppc test/test_loop.cfg
```

Exit code 0 = all tests passed. Nonzero = number of failures.

## Test programs

| Test | Config | Description |
|------|--------|-------------|
| `test_loop.S` | `test_loop.cfg` | Hello world + counting loop. Basic JIT validation. |
| `test_alu.S` | `test_alu.cfg` | 51 ALU tests: addi, addis, add, subf, ori, and, xor, slw, srw, neg, mullw, oris, xori, xoris, or, stw/lwz, stb/lbz, sth/lhz, rlwinm, cmp/branch, mtspr/mfspr, mfcr/cmpwi, mfmsr, mtmsr, mulli, mulhwu, rlwimi, nor, orc, cntlzw, subfic, lwbrx, stwbrx, divwu, addic, adde, subfe, srawi, lwarx/stwcx. reservation semantics. |
| `test_mem.S` | `test_mem.cfg` | 18 memory tests: word/half/byte store/load, byte-in-word extraction, multi-page stride, 1024-word XOR loop, cross-size access patterns. |
| `test_dsi.S` | `test_dsi.cfg` | DSI exception handling: installs handler at vector 0x300, accesses unmapped pages to trigger DSI, handler creates PTE and returns via rfi, verifies retry succeeds. Tests lwz/stw/sth/stb/lhz/lbz through DSI-mapped pages. |
| `test_multiple_dsi.S` | `test_multiple_dsi.cfg` | Cross-page `lmw/stmw`: a successful TLB slow-path call precedes a DSI on the next page. Verifies SRR0, DSISR, retry and transferred data; exit 100 indicates a wrong fault PC. |
| `test_branch_loop.S` | `test_branch_loop.cfg` | 20 branch tests: counted loops with `bl` calls inside (same-page `ble` + `bl` dispatch), `bdnz`/`bdz`, `bctr`/`bctrl`/`blrl`, conditional `bclr` variants (`beqlr`, `bnelr`, `bltlr`, `bgelr`, `bgtlr`, `blelr`) testing both taken and not-taken paths. |
| `test_fpu_arith.S` | `test_fpu_arith.cfg` | 48 FPU tests: fabs, fnabs, fadd/fsub/fmul/fdiv (double+single), fmadd/fmsub/fnmadd/fnmsub (double+single), fsqrt, fcmpu, frsp, fctiwz, fsel, lfs/stfs/lfsu/stfsu (single↔double conversion, rA update), FPSCR rounding modes (mffs, mtfsfi, fdiv 10/3 under RN=0/1/2/3, negative under RN=3). |
| `test_fpu_exc.S` | `test_fpu_exc.cfg` | 24 FPU tests: NO_FPU exception handling (installs handler at 0x800, verifies lfd/fadd/stfd/fdivs/lfs/stfs raise NO_FPU when MSR_FP=0, checks SRR0/SRR1), NO_FPU vs DSI priority, fmr (64-bit copy), fneg (sign bit flip for +val, -val, -0.0). |
| `test_altivec.S` | `test_altivec.cfg` | 12 AltiVec tests: MSR_VEC enable via rfi, vxor, vspltisw, vspltisb, vadduwm, vsubuwm, vand, vaddubm, vmrghw, vcmpequw. (with CR6), lvx/stvx round-trip, vspltw. |
| `test_crlogical.S` | `test_crlogical.cfg` | CR logical operations: crand, crandc, cror, crorc, crxor, crnand, crnor, creqv, plus crclr/crset aliases. |

## AArch64 code generation at fragment boundaries

After a normal macOS arm64 build, run:

```sh
bash test/run_aarch64_codegen_tests.sh
```

This executes the actual `stwcx.` generator at every aligned fragment boundary,
with the next fragment 2 MiB ahead or behind. The 3,072 cases check both indexed
address forms, XER.SO clear/set, absent/matching/stale reservations, CR preservation,
memory writes and MMU call counts. MMU calls use test doubles; the emitter and
generated AArch64 instructions are real. Other hosts skip this native execution test.

## MacIO SCC console regression

After configuring the project, run this host test from the repository root:

```sh
c++ -std=c++11 -DHAVE_CONFIG_H -I. -Isrc test/test_macio_scc.cc src/io/macio/scc.cc -o /tmp/test_macio_scc
/tmp/test_macio_scc
```

It exercises the two SCC channels, register selection and reset, baud register readback,
and Darwin's poll/write/poll console sequence, including captured output bytes.

## Writing a new test

1. Create `test_foo.S` with `_start` as entry point
2. Use the `PRINT` and `CHECK`/`CHECK32` macros from existing tests
3. Keep a fail counter in `r30`, exit with `.long 0x00333304`
4. Create `test_foo.cfg` (copy from `test_alu.cfg`, change `prom_loadfile`)
5. Build with Docker (see above)
6. All linker scripts use `test_loop.ld` (loads at `0x100000`)

## Linker script

`test_loop.ld` places code at `0x100000` and data at `0x200000`:
```
ENTRY(_start)
SECTIONS {
    . = 0x100000;
    .text : { *(.text) }
    . = 0x200000;
    .data : { *(.data) }
    .bss  : { *(.bss) }
}
```

`run_aarch64_codegen_tests.sh` also runs `test_branch_state.cc`: 1,280 native
branch executions check that the former XNU `mapDrainBusy` instruction pattern
does not invoke an OS-specific state-modification hook. This prevents silently
rewriting guest mapping counts when an ordinary branch is translated.

## BAT mapping regression tests

`test_bat_tlb.S` verifies data-cache translation invalidation when a DBAT is
installed, revoked, retargeted, or resized. Six checks cover both stale PTE and
stale BAT translations, plus recalculation of BRPN when BL changes. The data
segment has a PROM-created PTE; its effective address must not be assumed to
match its physical address. `test_bat_code.S` switches IBAT1L while executing
inside its mapped block and checks that the next instruction comes from the
new physical page. Both are included in `run_tests.sh`.

Rebuild with `test/build_ppc_elf.sh test_bat_tlb` or
`test/build_ppc_elf.sh test_bat_code`. Their small ELF fixtures are included.

## Runtime optical-media regression (macOS)

After building normally, run `bash test/run_cd_media_tests.sh`.
It exercises the real IDE packet/PIO/DMA paths, with only guest DMA memory and
PIC interrupts replaced by test doubles. Cases cover optical numbering when
the drive is IDE slave, queued changes during PIO and the DMA start handshake,
unit attention and request-sense clearing, failed opens, invalid images,
media locks, guest/host eject, reinsert, independent optical drives, configured
DVD identity, and data integrity before and after a swap. Fixtures are small
temporary images. The native SDL file-picker interaction still needs a GUI check.

## Targeted guest-PC snapshots (AArch64)

Set `PEARPC_TRACE_PCS` to up to 32 comma-separated, aligned hexadecimal guest
**effective** addresses and `PEARPC_TRACE_PC_FILE` to a CSV path. The JIT logs
stored CPU registers at matching addresses, capped at 20,000 records. Set
`PEARPC_TRACE_USER_ONLY=1` to exclude supervisor-mode hits. Register tracing
alone does not read guest memory. Optional `PEARPC_TRACE_SNAPSHOT_FILE`,
`PEARPC_TRACE_SNAPSHOT_PC` and `PEARPC_TRACE_SNAPSHOT_OCCURRENCE` capture RAM and
MMU/register JSON once at the selected probe; other I/O threads may still run.
`run_with_crash_capture.sh` accepts `auto` for either output file to keep it in
the run directory, and `PEARPC_BINARY` selects an alternate executable. The Tiger-specific launcher and its
version-dependent addresses are documented in `doc/OSX104_KEYBOARD_SETUP_20261004.md`.

## AltiVec whole-vector shifts

`bash test/run_vector_shift_tests.sh` builds the actual AArch64 fallback and
portable interpreter handlers with UBSan, and compares both to an independent
byte-stream oracle: shifts 0..7 in both directions, 256 deterministic input
vectors, and destination aliasing either source (12,288 cases per backend).
The zero-shift case must not merge the two 64-bit halves.

`test_vec_shift_zero.S` additionally exercises instruction decode, AltiVec
loads/stores and the normal CPU execution paths through a bare-metal ELF. It is
included in `run_tests.sh`; rebuild with `test/build_ppc_elf.sh test_vec_shift_zero`.
