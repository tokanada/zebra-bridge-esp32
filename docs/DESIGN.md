# zebra-bridge-esp32 — Design Specification

ESP32 port of [zebra-bridge](../../zebra-bridge) (Raspberry Pi, BlueZ). The ESP32
emulates a Zebra **ZD421** Bluetooth LE printer so the Rock RMS Check-in iPad
app can print, and forwards the raw ZPL bytes over **RS-232** (via a MAX3232
level-shifter board) to a Zebra **GX420d**.

No pairing, no bonding, no Bluetooth Classic SPP.

## Supported boards

| PlatformIO env | Board | Printer UART | TX pin | RX pin | CTS pin | LED |
| --- | --- | --- | --- | --- | --- | --- |
| `esp32dev` | ESP32-WROOM-32 DevKit | UART2 (`Serial2`) | GPIO17 | GPIO16 | -1 (off) | GPIO2, active high |
| `esp32c3` | ESP32-C3 DevKitM-1 / SuperMini | UART1 (`Serial1`) | GPIO5 | GPIO4 | -1 (off) | GPIO8, active low |

Notes:
- The USB console is `Serial` (UART0 on WROOM; native USB-CDC on C3 with
  `ARDUINO_USB_CDC_ON_BOOT=1`). Never use UART0 / GPIO20/21 on the C3 for the
  printer: the ROM bootloader prints to them at reset and the printer would
  receive garbage.
- Pins are compile-time defaults in `include/board_config.h`, selected with
  `#if CONFIG_IDF_TARGET_ESP32C3` / `#else`, and each can be overridden at
  runtime through the console (stored in NVS).

## BLE profile (must match the Pi bridge byte-for-byte)

Primary service `38eb4a80-c570-11e3-9507-0002a5d5c51b` (Zebra parser service):
- `38eb4a82-c570-11e3-9507-0002a5d5c51b` — WRITE | WRITE_NR. Every write
  value is appended to the print queue. Max attribute length 512 bytes;
  long (prepared) writes must work.
- `38eb4a81-c570-11e3-9507-0002a5d5c51b` — READ | NOTIFY. Read returns the 3
  bytes `0x02 '0' 0x03`. Bytes received from the printer's RS-232 TX (except
  XON/XOFF when software flow control is on) are sent as notifications to a
  subscribed client, chunked to (MTU - 3).

Device Information Service `0x180A`, all READ, UTF-8 strings:
- 0x2A29 Manufacturer = `Zebra Technologies`
- 0x2A24 Model = `ZD421`
- 0x2A25 Serial = configured serial (default `ZD421-EMULATION-0001`)
- 0x2A26 Firmware = `V1.0.0`
- 0x2A27 Hardware = `1.0`
- 0x2A28 Software = `1.0`

GAP / advertising:
- GAP device name = configured name (default `ZD421-Emulation`).
- Advertising packet: flags + complete 128-bit service UUID list (Zebra svc) +
  TX power. Scan response: complete local name. (128-bit UUID + name do not
  fit in 31 bytes together.)
- Connectable, general discoverable. Restart advertising on disconnect.
- Preferred ATT MTU 517.
- On connect, request connection params min 12, max 24 (×1.25 ms),
  latency 0, supervision timeout 500 (×10 ms) — iOS-friendly, same as the Pi.
- No security / no bonding (`setSecurityAuth(false,false,false)`).
- Single connection is enough; allow a new client after disconnect.

## Data path and back-pressure

```
BLE onWrite (NimBLE host task) --> FreeRTOS StreamBuffer (32 KiB default)
     --> printer TX task --> HardwareSerial --> MAX3232 --> GX420d
GX420d TX --> MAX3232 --> printer RX task --> XON/XOFF filter --> notify
```

- The GX420d at 9600 baud drains ~960 B/s, far slower than BLE, so the
  stream buffer absorbs whole labels.
- If the buffer lacks space, `onWrite` blocks (`xStreamBufferSend` with a
  timeout of `WRITE_BLOCK_TIMEOUT_MS`, 15000) — this stalls the NimBLE host,
  which is the intended back-pressure for both write types. Bytes not
  accepted after the timeout are dropped and logged as an error.
- Printer TX task writes in chunks of at most 64 bytes and pauses while a
  software-XOFF (0x13) has been received and not yet released by XON (0x11).
  An XOFF that lasts longer than 60 s is logged as a warning each minute but
  does not drop data.
- Optional hardware flow control: if a CTS pin is configured, use
  `setPins(rx, tx, cts, -1)` and `setHwFlowCtrlMode(UART_HW_FLOWCTRL_CTS)`.
- Serial format: configurable baud (default 9600), 8N1. Flow control modes:
  `none`, `xonxoff` (default), `cts`.

## Configuration

Configuration is layered, highest priority last:

1. Compile-time board defaults in `include/board_config.h`, selected by target.
2. Build-time overrides from `bridge.ini` (`[bridge]`, plus the section named
   after the PlatformIO env, `[esp32dev]` or `[esp32c3]`).
3. Runtime values saved by the console in NVS (`Preferences`, namespace
   `zbridge`).

`scripts/bridge_config.py` is a PlatformIO `pre` extra script on the ESP32
envs only (not `native`). It reads `bridge.ini` from the project root, or the
file named by `ZB_CONFIG` (relative to the project root or absolute; a missing
explicit file is a build error). A missing default `bridge.ini` means "no
overrides" and prints one notice line. The file is UTF-8 with optional BOM,
accepts CRLF, and treats `;`/`#` as comments (inline comments must follow
whitespace).

All parsing, validation, and header rendering are pure Python functions so
`tests/test_bridge_config.py` can run them without SCons. Validation mirrors
`console.cpp`: name 1..29 printable ASCII, serial 1..32 printable ASCII, the
same baud list, `flow` ∈ `none|xonxoff|cts`, `log` ∈ `error|info|debug`,
`ledlow` ∈ `0|1`, and the per-target GPIO tables (`cts`/`led` also accept
`-1`). Any invalid value or unknown key/section stops the build with a message
naming the file, key, and reason.

The script writes `$BUILD_DIR/generated/bridge_config_gen.h` (rewritten only
when its content changes) and adds that directory to `CPPPATH`. For each key
present it defines `ZB_CFG_HAS_<KEY> 1` and `ZB_CFG_<KEY>` (strings as escaped
C literals; `flow`/`log` as the integer values of `console::FlowMode` /
`console::LogLevel`). It always defines `ZB_CFG_SOURCE` (path relative to the
project root, or `none`) and `ZB_CFG_HASH`, the FNV-1a 32-bit hash of the
sorted normalised `key=value\n` lines (empty-string hash when there is no
file). `defaultConfig()` includes the header via `__has_include`, so the
`native` build and any missing header behave as "no overrides".

`ConfigStore::begin()` reads the NVS `cfghash` key (UInt). If it is absent or
differs from `ZB_CFG_HASH`, it clears the namespace, writes the new hash, and
logs `config: applied <source> (hash 0x...), cleared saved console settings`;
otherwise the saved console values win. `reset-config` clears the user keys
and then re-writes `cfghash`. `show` starts with
`config source: <source> hash 0x........`. Flashing a changed file therefore
resets console changes, re-flashing the same file keeps them, and
`pio run -t erase` wipes NVS entirely.

Keys: `name`, `serial`, `baud`, `flow`, `tx`, `rx`, `cts`, `led`, `ledlow`,
`log`. Missing keys fall back to `board_config.h` defaults.

USB console (115200 baud on WROOM), line based, case-insensitive commands,
CR, LF, or CRLF terminated, max line 128 chars:

```
help
show                      print resolved config + runtime stats
set name <text>           1..29 printable ASCII chars
set serial <text>         1..32 printable ASCII chars
set baud <n>              one of 1200 2400 4800 9600 14400 19200 28800 38400 57600 115200
set flow <none|xonxoff|cts>
set tx|rx|cts|led <gpio>  -1 disables cts/led; validated against the target's valid GPIOs
set ledlow <0|1>
set log <error|info|debug>
test                      queue a small ZPL test label (^XA ... ^XZ)
reset-config              erase the namespace
reboot
```

`set` persists immediately and prints "saved; reboot to apply" (name/serial/
pins/baud require a reboot). The parser lives in `src/console.cpp` /
`include/console.h` with **no Arduino dependencies** in the parse/validate
layer so it can be unit-tested on the host (`pio test -e native`).

Stats in `show`: uptime, connected (y/n) + peer, MTU, bytes received over BLE,
bytes sent to printer, bytes dropped, queue fill, xoff state, writes count.

## Logging

`log` level `info` default: connect/disconnect, MTU, job summary (a "job" is
a burst of writes; summarize bytes when 2 s pass without a write).
`debug`: every BLE write length plus first 64 bytes as escaped text.

## Status LED

- Fast blink (100 ms) — booting / BLE init failed.
- Slow blink (1 s) — advertising, no client.
- Solid on — client connected.
- Brief off-flicker on data to printer.

## Robustness

- Enable task watchdog for the main loop and the printer TX task.
- If BLE init fails, retry 3 times then `esp_restart()`.
- Every 10 s: if not connected and not advertising, restart advertising
  (mirrors the Pi bridge's health check).

## Wiring

```
ESP32 3V3 --> MAX3232 VCC      ESP32 GND --> MAX3232 GND
ESP32 TX  --> MAX3232 TTL TXD/DIN   (RS-232 side TX goes to printer pin 2 RXD)
ESP32 RX  <-- MAX3232 TTL RXD/ROUT  (RS-232 side RX comes from printer pin 3 TXD)
DB9 pin 5 = GND
```
The GX420d DB9 is DTE-wired; most MAX3232 DB9 boards are also DTE, so a
**null-modem** cable/adapter is usually required. Printer serial settings
must match (`^SC9600,8,N,1,X` or via Zebra Setup Utilities).

## Project layout

```
platformio.ini            envs: esp32dev, esp32c3, native (tests only)
bridge.ini.example        documented template (bridge.ini is git-ignored)
scripts/bridge_config.py  INI parsing/validation + header generation
include/board_config.h    per-target defaults
include/console.h         pure C++ command parser / validators
include/flow_filter.h     pure C++ XON/XOFF filter state machine
src/main.cpp              setup/loop, LED, health check
src/ble_printer.cpp/.h    NimBLE GATT server, advertising, callbacks
src/printer_link.cpp/.h   StreamBuffer, UART, TX/RX tasks, stats
src/config_store.cpp/.h   Preferences load/save, config hash handling
src/console.cpp           parser implementation
src/flow_filter.cpp
test/test_console/        Unity tests for parser/validators
test/test_flow_filter/    Unity tests for XON/XOFF filter
tests/test_bridge_config.py  pytest tests for the build-time config
README.md
```

Dependencies (pinned): `platform = espressif32@6.9.0`,
`framework = arduino`, `lib_deps = h2zero/NimBLE-Arduino@2.1.0`.
NimBLE build flags: `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`,
`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517`.
