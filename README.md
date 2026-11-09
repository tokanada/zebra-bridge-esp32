# zebra-bridge-esp32

An ESP32 firmware that makes a **Zebra GX420d** (RS-232 only) look like a
**Zebra ZD421** BLE printer to phones, tablets, and desktop apps. The ESP32 is a
BLE peripheral using the same GATT profile as the Raspberry Pi bridge; every
byte written by the client is forwarded unchanged over UART/RS-232 to the
printer. Bytes sent back by the printer (status, `~HS`, etc.) are filtered for
XON/XOFF and notified back to the BLE client.

```
BLE client --write--> ESP32 BLE --32 KiB buffer--> UART TX --> MAX3232 --> GX420d
BLE client <--notify-- ESP32 BLE <--XON/XOFF filter-- UART RX <-- MAX3232 <--
```

## Supported boards

| PlatformIO env | Board | Printer UART | TX pin | RX pin | CTS pin | LED |
| --- | --- | --- | --- | --- | --- | --- |
| `esp32dev` | ESP32-WROOM-32 DevKit | UART2 (`Serial2`) | GPIO17 | GPIO16 | -1 (off) | GPIO2, active high |
| `esp32c3` | ESP32-C3 DevKitM-1 / SuperMini | UART1 (`Serial1`) | GPIO5 | GPIO4 | -1 (off) | GPIO8, active low |
| `esp32s3` | ESP32-S3 DevKitC-1 / SuperMini | USB host (native OTG) | GPIO19 (D-) | GPIO20 (D+) | n/a | GPIO48 (WS2812) |

The ESP32-C3 console is the native USB CDC serial port
(`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`). Never point the printer
UART at UART0 / GPIO20 / GPIO21 on the C3: the ROM bootloader prints there at
reset and the printer would receive garbage.

The ESP32-S3 build uses the native USB OTG peripheral as a USB **host** and
talks to the printer's USB-B port directly, with no UART or MAX3232 in the
path. See [ESP32-S3 (USB printer)](#esp32-s3-usb-printer).

## BLE profile

The device advertises a Zebra-compatible GATT service:

- Service `38eb4a80-c570-11e3-9507-0002a5d5c51b`
- Write characteristic `38eb4a82-c570-11e3-9507-0002a5d5c51b`
  (`WRITE | WRITE_NR`, 512-byte attribute, long/prepared writes accepted)
- Read characteristic `38eb4a81-c570-11e3-9507-0002a5d5c51b`
  (`READ | NOTIFY`, initial value `02 30 03`; printer replies are notified in
  `MTU - 3` byte chunks, only when the client subscribes)

Advertising data contains the flags, the complete 128-bit service UUID, and
the TX power level. The scan response contains the complete local name. Both
fit in the 31-byte legacy advertising limit.

The Device Information service (`0x180A`) exposes Zebra Technologies / ZD421 /
the configured serial / version strings.

## Wiring

```
ESP32 3V3 --> MAX3232 VCC      ESP32 GND --> MAX3232 GND
ESP32 TX  --> MAX3232 TTL TXD/DIN   (RS-232 side TX goes to printer pin 2 RXD)
ESP32 RX  <-- MAX3232 TTL RXD/ROUT  (RS-232 side RX comes from printer pin 3 TXD)
DB9 pin 5 = GND
```

The GX420d DB9 is DTE-wired; most MAX3232 DB9 boards are also DTE, so a
**null-modem** cable/adapter is usually required. A straight-through cable
will often appear to "work" for transmit but deliver nothing back, or nothing
at all.

Optional hardware flow control (CTS): wire the printer's RTS/CTS as required
for your adapter, set a CTS GPIO with `set cts <gpio>` and `set flow cts`.
The current board defaults leave CTS disabled and use XON/XOFF.

## ESP32-S3 (USB printer)

Build and flash with the ESP32-S3 env:

```sh
pio run -e esp32s3 -t upload
```

The ESP32-S3's native USB OTG peripheral is the printer port: the board acts
as a USB host. Wire it straight to the GX420d's USB-B socket:

| ESP32-S3 | USB-B pin | Signal | Wire colour |
| --- | --- | --- | --- |
| 5V | 1 | VBUS | red |
| GPIO19 | 2 | D- | white |
| GPIO20 | 3 | D+ | green |
| GND | 4 | GND | black |

Keep the D+ / D- wires short and twisted together.

Because the S3's own USB port is now the host port, the console and logs are
on UART0: GPIO43 (TX) and GPIO44 (RX) at 115200 baud, through a **3.3 V**
USB-UART adapter.

### Debugging while connected to the printer (default method)

Use an ESP32-WROOM-32 DevKit as the USB-UART adapter: its CP2102/CH340 bridge
talks to the S3 directly while the WROOM itself is held in reset. No sketch is
needed on the WROOM.

| WROOM DevKit pin | Connect to |
| --- | --- |
| EN | GND on the WROOM (jumper; holds the WROOM in reset) |
| TX (TX0 / GPIO1) | S3 GPIO43 (TX) |
| RX (RX0 / GPIO3) | S3 GPIO44 (RX) |
| GND | S3 GND |
| 5V / 3V3 | not connected (the S3 is powered from the printer side) |

The wiring is **straight through** (TX to TX, RX to RX) because the DevKit's
TX/RX labels are the ESP32's pins, and you are talking to the USB bridge behind
them. Plug the WROOM into the computer and open its port:

```sh
pio device monitor -b 115200 -p /dev/cu.usbserial-XXXX
```

Only TX/RX/GND are wired, so the monitor cannot auto-reset the S3; press the
S3's RST button to see the boot log. Useful commands: `set log debug` (USB
detail such as GET_DEVICE_ID), `show` (attach state, VID:PID — Zebra is
`0a5f` — and byte counters), and `test` (queue a ZPL test label without the
iPad).

The same link can reflash the S3 without unplugging the printer: hold the S3's
BOOT, tap RST, release BOOT, then
`pio run -e esp32s3 -t upload --upload-port /dev/cu.usbserial-XXXX` and tap RST
afterwards.

If output is garbled or missing, swap TX/RX and confirm the EN jumper is in
place (a booting WROOM collides with the S3's output).

Flashing and power:

- Flash with the printer disconnected: hold BOOT and plug in the USB-C cable.
- When the printer is connected, power the board from its 5V pin with the
  USB-C cable unplugged, so the S3's USB port stays dedicated to the printer.
- The status LED is the onboard WS2812 on GPIO48. A short 100 ms blink every
  2 s means no printer is attached; solid on means a BLE client is connected.
- `baud`, `flow`, `tx`, `rx`, and `cts` are ignored on this build; USB bulk
  transfers NAK while the printer is busy, so no flow control is needed. Only
  `led`, `log`, `name`, and `serial` matter.

## Printer setup

Configure the GX420d serial port to match the bridge defaults:

- 9600 baud, 8 data bits, no parity, 1 stop bit, XON/XOFF flow control
- `^SC9600,8,N,1,X` in ZPL, or the equivalent in Zebra Setup Utilities
- If the printer is set to `^SC...,N` (no flow control), run `set flow none`
  on the bridge console as well.

## Configuration file

Settings live in `bridge.ini` at the project root. Copy the example, edit it,
and flash — no console commands are required:

```sh
cp bridge.ini.example bridge.ini
$EDITOR bridge.ini
pio run -e esp32dev -t upload
```

`bridge.ini` is git-ignored. `[bridge]` applies to every board; an optional
`[esp32dev]`, `[esp32c3]`, or `[esp32s3]` section overrides it for that
environment only. Only keys present in the file override the defaults in
`include/board_config.h`.
The format is INI/UTF-8 and `;` or `#` starts a comment. Each bridge needs a
unique `name` and `serial` so clients can tell devices apart.

For several bridges, keep one file per device and choose it with `ZB_CONFIG`
(a path relative to the project root, or absolute):

```sh
ZB_CONFIG=configs/lobby.ini pio run -e esp32c3 -t upload
```

If `ZB_CONFIG` is set, that file must exist. Without it, a missing
`bridge.ini` simply means "use the board defaults" (one notice line is printed
during the build).

Precedence is: board defaults < config file < console `set` changes made after
that flash. The build tool hashes the resolved configuration and stores the
hash in NVS. Flashing a file whose settings changed clears the saved console
changes; flashing the same file again keeps them. `pio run -t erase` wipes NVS
completely, including the hash.

## Build and flash

PlatformIO is required. The repository `.gitignore` excludes `.venv` and
`.pio`.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install platformio

# ESP32-WROOM-32
pio run -e esp32dev -t upload

# ESP32-C3 (native USB)
pio run -e esp32c3 -t upload

# ESP32-S3 (USB host printer)
pio run -e esp32s3 -t upload

# Open the serial console
pio device monitor -b 115200
```

(Without activating the venv, use `.venv/bin/pio` in place of `pio`.)

All builds use `platform = espressif32@6.9.0` and
`h2zero/NimBLE-Arduino@2.1.0`.

Run the host tests (no hardware needed):

```sh
pio test -e native
```

## Console

The console is line based at 115200 baud, case-insensitive, accepts `CR`,
`LF`, or `CRLF`, and limits input lines to 128 characters. On the C3 use the
USB CDC port; on the S3 use UART0 GPIO43/GPIO44 through a 3.3 V USB-UART
adapter (the native USB port is the printer host port).

| Command | Description |
| --- | --- |
| `help` | Show the command list |
| `show` | Show runtime statistics and the stored configuration |
| `set name <text>` | BLE name / advertised local name, 1-29 printable ASCII characters |
| `set serial <text>` | Device information serial number, 1-32 printable ASCII characters |
| `set baud <n>` | `1200 2400 4800 9600 14400 19200 28800 38400 57600 115200` |
| `set flow <none\|xonxoff\|cts>` | Printer flow control mode |
| `set tx <gpio>` / `set rx <gpio>` | Printer UART pins (validated for the target) |
| `set cts <gpio>` | CTS pin; `-1` disables hardware flow control |
| `set led <gpio>` | Status LED pin; `-1` disables the LED |
| `set ledlow <0\|1>` | LED polarity: `0` active high, `1` active low |
| `set log <error\|info\|debug>` | Console log level (default `info`) |
| `test` | Queue a small ZPL test label |
| `reset-config` | Erase the `zbridge` NVS namespace |
| `reboot` | Restart the ESP32 |

`set` saves immediately and prints `saved; reboot to apply`. Name, serial,
printer/LED pins, and baud take effect after `reboot`; `flow`, `log`, and
`ledlow` are applied immediately as well as being persisted.

`show` reports uptime, connection state and peer address, MTU, BLE bytes,
printer bytes, dropped bytes, queue fill, XOFF state, write count, and the
stored configuration.

## Status LED

| Pattern | Meaning |
| --- | --- |
| Fast blink (100 ms) | Booting or BLE initialization failed |
| Slow blink (1 s) | Advertising, no client connected |
| Solid on | Client connected |
| Brief off flicker | Data is being forwarded to the printer |
| Short blink (100 ms every 2 s) | ESP32-S3 USB build: no printer attached (while disconnected) |

On the `esp32s3` build the LED is the onboard WS2812 on GPIO48 and `ledlow`
has no effect.

## Troubleshooting

**The device does not appear in the BLE scan.** Check the console for
`error: BLE init ...` messages and make sure the LED is slow-blinking. The
firmware retries BLE initialization three times, then restarts. The loop also
restarts advertising every 10 seconds if the device is idle and not
advertising.

**The client connects but nothing prints.** Run `show` and check that `ble
bytes` increases. Then check the printer baud/format/flow settings and the
null-modem wiring. A MAX3232 board with the wrong TX/RX crossover receives
nothing.

**The printer prints garbage.** Verify the baud rate, 8N1, and the flow mode.
On the C3 do not move the printer UART to GPIO20/21 or UART0. Check that the
MAX3232 is a 3.3 V-compatible board and shares ground with the ESP32.

**Jobs stall or time out.** The 32 KiB buffer provides back-pressure: when
the printer is slower than BLE, writes block for up to 15 seconds and then the
unaccepted remainder is dropped and logged. Large jobs should be sent in
chunks and the client should tolerate flow control. If XON/XOFF is enabled,
the bridge pauses its UART transmitter while the printer is asserting XOFF
and warns once a minute if XOFF lasts longer than 60 seconds.

**The test label prints but the client's labels do not.** Confirm the client
is writing to the Zebra write characteristic, not the read/notify
characteristic, and that it sends raw ZPL bytes. The bridge performs no
translation.

**Cannot flash the ESP32-C3.** Use the USB-C port wired to the native USB
peripheral, not a UART bridge. If needed, hold BOOT while resetting, then
upload. The console appears as a USB CDC device.

**S3 build: the printer is not detected.** `show` reports
`printer: usb not attached`. Check the four wires to the USB-B plug (5V, D-,
D+, GND), that the printer is powered, and that the cable is a real USB data
cable. Keep D+/D- short and twisted. D- is GPIO19 and D+ is GPIO20.

**S3 build: no console output.** The console is UART0 on GPIO43 (TX) and
GPIO44 (RX) at 115200; connect a 3.3 V USB-UART adapter. The S3's USB-C port
is the printer host and does not provide a CDC console on this build.

**No output on the console.** On the `esp32dev` board the console is UART0
(GPIO1/GPIO3) through the USB-UART bridge; on the C3 it is native USB CDC.
Open the matching port at 115200 baud.
