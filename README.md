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

The ESP32-C3 console is the native USB CDC serial port
(`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`). Never point the printer
UART at UART0 / GPIO20 / GPIO21 on the C3: the ROM bootloader prints there at
reset and the printer would receive garbage.

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

## Printer setup

Configure the GX420d serial port to match the bridge defaults:

- 9600 baud, 8 data bits, no parity, 1 stop bit, XON/XOFF flow control
- `^SC9600,8,N,1,X` in ZPL, or the equivalent in Zebra Setup Utilities
- If the printer is set to `^SC...,N` (no flow control), run `set flow none`
  on the bridge console as well.

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
USB CDC port.

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

**No output on the console.** On the `esp32dev` board the console is UART0
(GPIO1/GPIO3) through the USB-UART bridge; on the C3 it is native USB CDC.
Open the matching port at 115200 baud.
