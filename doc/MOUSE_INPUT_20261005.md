# Mouse input support

## Current behavior

The host event structure uses a stable semantic mapping:

```text
button1 = left
button2 = right
button3 = middle
```

The SDL backend previously assigned SDL middle to `button2` and SDL right to
`button3`, while X11 and Win32 used the semantic mapping above. SDL now maps
right to `button2` and middle to `button3`.

CUDA emits ADB mouse register-0 packets. The packet format depends on the
handler selected by the guest:

- Legacy handlers keep the existing four-byte CUDA packet and two button bits.
- Handler 4 (used by Linux 2.4 `adbhid`) receives an extended packet. The
  first movement byte carries left/Y, the second carries middle/X, and the
  third carries right. Button bits are active-low.

The implementation chooses the packet length from `gCUDA.mousehandler`, so the
Linux three-button path does not change the legacy macOS path.

## Validation

`test/run_cuda_keyboard_tests.sh build/a64` now drives real CUDA register
transfers and checks 1,090 cases. It covers key press/release completion,
command/reply packets, the legacy mouse packet, and handler-4 packets for all
three button bits.

## Wheel support

The current guest-visible ADB handlers do not provide a portable wheel field:
handler 4's third byte carries button and high motion bits, not a scroll delta.
The SDL backend should not encode wheel events as arbitrary button transitions.
A future wheel implementation needs a guest-specific HID/ADB extension or an
explicit compatibility mode, followed by Linux and OS X guest tests.
