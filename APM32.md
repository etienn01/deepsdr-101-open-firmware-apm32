# APM32F407 port — HamGeek DSP-01 and other SDR101 clones

This branch (`apm32`) runs the [DEEPSDR 101 open firmware](https://github.com/gmtii/deepsdr-101-open-firmware)
on DeepSDR 101 clones built around the **Geehy APM32F407VGT6** instead of a GD32F450/F470. These clones are sold
as "SDR101", "SDR 101" or under brands such as HamGeek (DSP-01).

**Tested on one unit only.** Other APM32-based clones should work but are unverified: back up first.

## Is this for my radio?

Open the case and read the microcontroller's marking:

| Marking | What to use |
|---|---|
| GD32F450VET6 or GD32F470VGT6 | The upstream firmware, not this branch |
| APM32F407VGT6 | This branch |
| STM32F405 / STM32F407 | Untested; this branch is the closest match |

On the tested unit, the parts that could differ on another clone:

| Part | On the tested unit |
|---|---|
| Stock bootloader | Screen shows "Bootloader v4.0.3 LCD:0007 by BH5HNU" (encoder held at power-on) |
| Main crystal (HSE) | 12.288 MHz |

A different bootloader, crystal or panel may give no USB, wrong audio or a black screen. The ROM DFU below always
lets you go back.

## The USB fix

Many clones ship with **R8**, one of the two series resistors on the USB data lines, at 2.4 kΩ (marked "242")
instead of about 22 Ω: the computer sees no device at all. Replace it with 22 Ω.

## First flash: the ROM DFU (BOOT0)

The first flash goes through the microcontroller's built-in DFU loader, which needs BOOT0 (pin 94) high at
power-on. On the HamGeek, BOOT0 is pulled down by **R11** (10 kΩ, marked "103"): connect the chip side of R11 to
3.3 V (for example the tab of the LD1117 regulator) while switching on. The computer then sees
`314b:0106 Geehy APM32 ISP DFU mode`.

## Back up first

With the radio in ROM DFU, read the flash twice and compare the two files:

```
dfu-util -d 314b:0106 -a 0 -s 0x08000000:1048576 -U flash_backup.bin
```

Keep it: it holds the stock bootloader and firmware, and it is the only way back.

## Build

Requirements: the Arm GNU Toolchain (`arm-none-eabi-gcc`, tested with 15.3.rel2), `make` and `python3`.

```
make apm32                         # 144.384 MHz, within the chip's 168 MHz rating
make apm32 APM32_SYSCLK_MHZ=192    # 192 MHz, overclocked
```

Both worked on the tested unit; start with 144 on a new one. Run `rm -rf build_apm32` before switching between
them: `make` does not notice the change.

## Flash

```
dfu-util -d 314b:0106 -a 0 -s 0x08020000:leave -D build_apm32/firmware_apm32.bin
```

The stock bootloader stays in place and starts the firmware.

## Later updates, case closed

Two ways back into the ROM DFU without opening the radio, then the same `dfu-util` command:

- **From the menus**: Information → DFU mode.
- **Rescue gesture**, if the firmware no longer starts: switch on while **turning the encoder, without pressing
  it**, for about 3 seconds. The screen stays on "Running APP..." and the computer sees the DFU device.

## Never use update4.bin

Holding the encoder pressed at power-on starts the stock bootloader's USB disk mode, which looks for an
`update4.bin` file. On the APM32 that path erases the firmware and then hangs: only BOOT0 brings the radio back.
Never copy `update4.bin`, or the upstream `update.bin`, to that disk.

## Missing modes

The APM32 has 128 KB of main RAM instead of 192: **FT8, WSPR, HFDL, STANAG and the maps** are left out for now.
Everything else is the upstream firmware, documented in [README.md](README.md).

## Back to the stock firmware

Enter the ROM DFU with BOOT0, then:

```
dfu-util -d 314b:0106 -a 0 -s 0x08000000:leave -D flash_backup.bin
```
