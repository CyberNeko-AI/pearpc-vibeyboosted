# Shadow builds (Autotools / VPATH)

## Scope

PearPC supports separate source and build directories with the existing
Autotools build. Bootstrap (`autogen.sh`) still prepares generated build-system
files in the source tree. Configure, compile and test then run in separate build
directories. There is no need to introduce CMake for this separation.

Use the commands in [README.md](../README.md#shadow-builds). An already configured
in-place source tree must first be cleaned with `make distclean`; `make clean`
alone leaves `config.status` and is insufficient. This cleanup removes the old
in-place binary, so it is a deliberate migration step, not something the build
silently performs. Runtime VM paths remain relative to the launch directory.

## Changes

- Replace working-directory-relative header paths (`-I ../..`, etc.) with
  `-I$(top_builddir)/src -I$(top_srcdir)/src`. Generated headers are found in the
  build tree and checked-in headers in the source tree. Automake supplies the
  build-root include path for that build's `config.h`.
- Keep the existing Automake parser/lexer generation rules. Clean parallel builds
  generate `debugparse.c`, `debugparse.h` and `lex.c` in the build tree.
- Resolve Win32 resource includes against the source and build trees; resolve the
  BeOS generated Ethernet-source symlink against the actual POSIX source file.
- Make `autogen.sh` locate its own source directory, even if called elsewhere.
- Make `make test` build and select the current build's executable. Resolve test
  fixtures against the runner's source tree; isolate runtime writes in a temporary
  directory. Host codegen, optical-media and vector tests accept a build directory
  to select its headers and, where needed, its objects.
- Ignore the conventional root `build/` directory.

## Validation on 2026-10-04

Host: macOS arm64, Clang, SDL3. Validation used a clean source snapshot with the
working changes applied, outside the user's configured checkout. No VM disk
images were copied or changed.

Before the fix, a clean AArch64 VPATH build failed immediately with missing
`tools/snprintf.h`, `system/types.h` and `debug/tracers.h`.

After the fix, two independent build directories were configured against the
same bootstrapped source tree:

| Configuration / check | Result |
| --- | --- |
| AArch64 JIT + SDL, `make -j4` | Passed |
| Generic interpreter + SDL, `make -j4` | Passed |
| AArch64 `make test` | 16 passed, no failures or skips |
| Generic `make test` | 16 passed, no failures or skips |
| Native AArch64 codegen regressions using shadow-build objects | 3,072 stwcx. and 1,280 branch executions passed |
| UBSan vector shift tests using shadow-build configuration | 12,288 cases per backend passed |
| Optical-media controller tests using shadow-build objects | 10,688 checks passed |
| Source file sizes and nanosecond mtimes before/after both builds and tests | No added, removed or changed files |

These checks verify the supported build paths on the available host. They are not
an OS X installation test, a `make distcheck` packaging audit, or validation of the
legacy Win32/BeOS/X11/Qt/GTK targets on their native platforms.

## Build-system choice

Retain Autotools for this change: it already expresses the CPU, OS and UI choices,
static libraries, assembly and generated parser rules. The observed blocker was
path handling in project rules, not the choice of build system. A CMake migration
can be evaluated separately if the project needs different IDE or toolchain
integration; it would require reproducing and validating those platform rules.
