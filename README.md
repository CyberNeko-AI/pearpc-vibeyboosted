# PearPC - PowerPC Architecture Emulator

PearPC is an architecture-independent PowerPC platform emulator capable of running most PowerPC operating systems.

## Features

- **CPU emulation**: G3/G4 with AltiVec support
  - **Generic interpreter**: Portable to any host architecture (including ARM64/Apple Silicon)
  - **JITC x86**: JIT compiler for x86 hosts (~15x slower than native)
  - **JITC x86_64**: JIT compiler for x86_64 hosts
- **IDE controller** (CMD646 with bus-mastering): hard disks and CD-ROM via image files or host devices
- **PCI bridge**: enough to work with
- **Heathrow PIC**: programmable interrupt controller
- **VIA-CUDA**: with attached ADB keyboard and mouse
- **Network**: 3Com 3C90x or Realtek 8139 via host ethernet tunnel
- **NVRAM**: 8 KiB non-volatile storage
- **USB**: placeholder hub (sufficient for client OS detection)
- **Open Firmware PROM**: boots Yaboot and BootX from HFS/HFS+ partitions

## Tested Client Operating Systems

The following operating systems run (to some extent) inside PearPC:

- **Mandrake Linux 9.1 PPC**: Runs well
- **Darwin PPC**: Runs well
- **Mac OS X 10.3**: Runs well with some caveats

## Building

### Prerequisites

- C++ compiler with C++11 support (GCC or Clang)
- GNU Autotools (autoconf, automake)
- pkg-config
- SDL3 development libraries

On macOS:
```sh
brew install autoconf automake sdl3 pkg-config libslirp
```

### Compile

```sh
./autogen.sh
./configure
make
```

The configure script auto-detects your platform. You can override with:
- `--enable-cpu=generic|jitc_x86|jitc_x86_64|jitc_aarch64`
- `--enable-ui=sdl|x11|win32|beos`

The binary is produced at `src/ppc`.

### Shadow builds

Builds outside the source directory use the existing Autotools setup; CMake is
not required. Bootstrap once in the source tree, then configure each build
independently. For example, from the repository root on Apple Silicon:

```sh
./autogen.sh
# If this checkout was previously configured/built in place, first run:
# make distclean
mkdir -p build/a64 build/generic
(cd build/a64 && ../../configure --enable-cpu=jitc_aarch64 --enable-ui=sdl)
make -C build/a64 -j4
make -C build/a64 test

(cd build/generic && ../../configure --enable-cpu=generic --enable-ui=sdl)
make -C build/generic -j4
make -C build/generic test
```

Use a source tree with no in-place configuration (`config.status`) or leftover
objects. If configure says the source directory is already configured, run
`make distclean` **in the source root**, then retry the separate configure step.
That removes the in-place executable and build configuration; keep any custom
configure options to reuse them. It does not delete VM images or configurations.
A freshly bootstrapped checkout needs no `distclean`.

Each build has its own `config.h`, generated parser, objects, archives and
`src/ppc`. To run a VM whose configuration uses relative image and `video.x`
paths, keep the working directory at the repository root:

```sh
./build/a64/src/ppc ppccfg.osx
# Existing crash-capture scripts can use the same binary:
PEARPC_BINARY="$PWD/build/a64/src/ppc" scripts/debug/run_with_crash_capture.sh ppccfg.osx
```

`build/` is ignored by Git. Build directories can also live outside the checkout;
invoke the source tree's `configure` by its absolute path in that case. Run
`make -C build/a64 clean` or `distclean` to clean only that build.
See [shadow-build validation](doc/SHADOW_BUILD.md) for implementation details and
[test instructions](test/README.md) for standalone host tests.

### Platform Notes

| Host | CPU Backend | UI Backend |
|------|-------------|------------|
| macOS (Apple Silicon) | jitc_aarch64 or generic | SDL3 |
| macOS (Intel) | jitc_x86_64 or generic | SDL3 |
| Linux (x86_64) | jitc_x86_64 | SDL3 or X11 |
| Windows | jitc_x86 or jitc_x86_64 | Win32 native |

## Quick Start

### 1. Get a client OS image

Download [Mandrake Linux 9.1 PPC](https://archive.org/details/MandrakeLinux9.1ppc) from the Internet Archive.

### 2. Run

Copy `ppccfg.example` and edit it, or use the provided `ppccfg.test` for a quick CD boot:

```sh
# Using a config file:
./src/ppc ppccfg.test

# Or directly from the command line:
./src/ppc --pci-ide0-master-installed --pci-ide0-master-type=cdrom \
          --pci-ide0-master-image=MandrakeLinux-9.1-CD1.ppc.iso \
          --prom-driver-graphic=video.x
```

## Usage

```
ppc [options] [configfile]
```

All configuration can be provided via config file, command line, or both. CLI options override config file values. The config file is optional if all needed values have defaults or are set via CLI.

### Command Line Options

| Option | Description |
|--------|-------------|
| `-c, --config <file>` | Config file path (alternative to positional arg) |
| `--<key>=<value>` | Set any config key (hyphens become underscores) |
| `--<key>` | Same as `--<key>=1` (boolean flag) |
| `-h, --help` | Show help with all available config keys |

Examples:
```sh
# Traditional: config file only
./src/ppc myconfig.cfg

# Override a config value from command line
./src/ppc --memory-size=0x10000000 myconfig.cfg

# No config file, all from CLI
./src/ppc --pci-ide0-master-installed --pci-ide0-master-type=cdrom \
          --pci-ide0-master-image=linux.iso --prom-driver-graphic=video.x

# Headless mode (no GUI window)
./src/ppc --headless myconfig.cfg
```

### Change discs while running (SDL)

Use **F10** to select a new image for the first installed optical drive, or
**Ctrl+F10** to eject it. For a second optical drive, use **Shift+F10** and
**Ctrl+Shift+F10**. On macOS, the Fn key may be needed to send function keys.
The first optical drive can be either IDE master or slave.

Canceling the picker or selecting an unreadable/invalid image preserves the old
disc. If the guest has locked it, eject/unmount it inside the guest first.
Pending changes wait for an active PIO/DMA transfer to finish; the guest then
receives a media-change notification on its next command. Images must contain
2048-byte sectors (e.g. ISO); raw 2352-byte BIN/CUE and compressed DMG files are
not supported by this image backend. The selected path is not saved to the
configuration: restart uses the configured image.

The shortcuts are configurable with `key_change_cd_0`, `key_change_cd_1`,
`key_eject_cd_0` and `key_eject_cd_1`. Use `"none"` to disable a shortcut.
Older configurations that explicitly set `key_change_cd_0 = "none"` need that
setting changed to enable the picker. Controls are available after PROM boot
selection completes. Runtime file selection requires the SDL UI (SDL 3.2+).

### Keyboard

Keypresses are sent directly to the client when the PearPC window is focused. PearPC uses a raw keyboard layout -- configure your preferred layout in the client OS.

Press **F11** to enter compose mode for key combinations that your host window manager intercepts (e.g., to send Ctrl+Alt+Del: press F11, then Ctrl, Alt, Del, then F11 again).

If the keyboard behaves strangely after Alt-Tab, press **Alt+Ctrl+Shift** to reset.

### Mouse

The client mouse is independent of the host mouse. Press **F12** to toggle mouse grab (switch between host and client mouse). The window title indicates the current mode.

The SDL backend sends left, right and middle buttons. For guests that select the
ADB extended mouse handler (Linux `adbhid` handler 4), PearPC sends the extended
three-button packet: Linux sees left, middle and right as separate buttons.
Older ADB handlers retain the two-button packet for compatibility. Mouse-wheel
scrolling is not exposed yet because the ADB packets used by the supported guest
kernels have no portable wheel field.

### Key Bindings

| Key | Action |
|-----|--------|
| F11 | Compose mode (for intercepted key combos) |
| F12 | Toggle mouse grab |
| Alt+Return | Toggle fullscreen |

These can be changed in the config file.

## Configuration Reference

PearPC uses plain text configuration files. See `ppccfg.example` for a fully commented example. All options can also be set from the command line (use `--help` to list all keys). Key options:

| Option | Default | Description |
|--------|---------|-------------|
| `ppc_start_resolution` | `"800x600x15"` | Window size and depth (15 or 32) |
| `ppc_start_full_screen` | `0` | Start in fullscreen mode |
| `redraw_interval_msec` | `20` | Redraw interval (10-500ms) |
| `memory_size` | `0x8000000` | RAM size (min 64 MiB) |
| `cpu_pvr` | `0x000c0201` | Processor version: G3=0x00080200, G4=0x000c0201 |
| `headless` | `0` | Run without display |
| `prom_bootmethod` | `"auto"` | Boot method: auto, select, or force |
| `prom_driver_graphic` | | Set to `"video.x"` for Mac OS X |
| `pci_ide0_master_installed` | `0` | Enable IDE master (HDD or CD-ROM) |
| `pci_ide0_master_image` | | Path to disk image (.img) or ISO (.iso) |
| `pci_ide0_master_type` | | `"hd"`, `"cdrom"`, or `"nativecdrom"` |
| `pci_ide0_slave_installed` | `0` | Enable IDE slave |
| `pci_3c90x_installed` | `0` | Enable 3Com NIC |
| `pci_rtl8139_installed` | `0` | Enable Realtek NIC |
| `nvram_file` | `"nvram"` | NVRAM storage file |
| `memdump_file` | | Memory dump file on exit/crash (empty = no dump) |
| `framebuffer_dump_file` | | Framebuffer dump file on exit/crash (empty = no dump) |

## Architecture

See [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) for a detailed description of the codebase.

## Authors

- **Sebastian Biallas** - Main developer
- **Stefan Weyergraf** - Major contributions
- **Daniel Foesch** - AltiVec and core updates

See [AUTHORS](AUTHORS) for the full list of contributors.

Contains code from: [HT Editor](https://hte.sf.net), Linux, [Bochs](https://bochs.sf.net), [MOL](https://www.maconlinux.org/), Yaboot, HFS/HFS+ utils.

## License

GPLv2. See [COPYING](COPYING).
