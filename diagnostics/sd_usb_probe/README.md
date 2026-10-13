# USB SD diagnosis — 2026-10-01

Tested the connected ESP32-S3 over `/dev/cu.usbmodem101`. The probe uses the
main firmware's pins: CS=10, MOSI=11, SCK=12, MISO=13. Wi-Fi is absent.
It does not create files, write to the card, or format it.

## Verified results

Two recorded runs gave the same results:

| Test | Result |
| --- | --- |
| Hardware SPI, 100 kHz, five CMD0 attempts | R1=0xFF on every attempt; expected 0x01 |
| Hardware SPI, 400 kHz, five CMD0 attempts | R1=0xFF on every attempt; expected 0x01 |
| Software SPI, approximately 12.5 kHz, five CMD0 attempts | R1=0xFF on every attempt; expected 0x01 |
| Arduino SD mount, formatting disabled | CMD0 receives no response; GO_IDLE_STATE fails; mount fails |
| I2C scan, SDA=8/SCL=9, 100 kHz | Devices respond at 0x1E and 0x6B |
| MISO with CS high and pull-up | 100/100 samples high |
| MISO with CS high and pull-down | 1/100 and 3/100 samples high |

The MISO tests concern a deselected card and do not prove a disconnected wire:
a correctly deselected SPI card can release its output. They do show that MISO
was not permanently stuck low during these runs.

## Interpretation

The SD fails at the first card command, before filesystem access. An unsupported
filesystem does not explain the missing CMD0 response. The fault persists with
Wi-Fi absent and with software-generated SPI, so Wi-Fi and the hardware SPI
controller are not sufficient explanations. The I2C peripherals are accessible
in the isolated probe, unlike the earlier main-firmware startup.

The remaining causes cannot be distinguished through terminal output alone:
incorrect physical SD pin mapping, missing module/card supply or ground, broken
tracks/solder joints, socket contact failure, missing/incorrect pull-ups, a failed
reader/level shifter, or a failed card. No supply voltage measurement was taken.
Do not infer the reader's required VCC from USB power or from these logs.

Next physical checks: verify continuity from the four configured GPIOs to the
reader's CS/DI/CLK/DO; measure module input and card supply against module ground;
identify the reader's regulator/level-shifter circuit; try a known-good card.
Change VCC only after confirming the module's input rating.

## Evidence and preservation

- `evidence/probe-usb.log`: first captured run.
- `evidence/probe-repeat-usb.log`: repeat run.
- `evidence/backup.json`: original 16 MB flash hash and replaced address range.
- `evidence/original-flash.bin`: complete original flash backup, locally retained
  and ignored by Git. Treat as private because it may contain device settings.
- `evidence/original-app-region.bin`: original sector-aligned bytes replaced by
  the diagnostic application (348160 bytes at address 0x10000).

Only the application range was replaced for testing; bootloader, partition
table, and all other flash addresses were preserved. The original application
range is restored after testing and verified against the backup.

To compile the probe: `pio run -d diagnostics/sd_usb_probe`. Loading it replaces the running
application temporarily. It starts when USB serial connects; send `R` to repeat.
