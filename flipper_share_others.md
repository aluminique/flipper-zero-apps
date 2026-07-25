# Flipper Share — other transport candidates (evaluation)

This document records the evaluation of every remaining transport candidate for
Flipper-to-Flipper file transfer that was **not** taken into work. Scope:

- Implemented: Sub-GHz (`flipper_share`), IR (`flipper_share_ir`), NFC
  (`flipper_share_nfc`).
- Specified for implementation: UART (`flipper_share_uart`), iButton
  (`flipper_share_ibutton`), LF RFID (`flipper_share_rfid`).
- Everything else: below.

Constraints applied throughout: both devices run **unmodified official firmware**, the
app builds with `ufbt` and ships as a `.fap`, and only SDK-exported symbols count
(verified against `targets/f7/api_symbols.csv`, API 87.x; firmware source checked on the
`dev` branch, 0.30.0-rc, and cross-checked against the released 1.4.3 SDK — the relevant
export sets are identical).

---

## BLE — NOT FEASIBLE flipper-to-flipper

**Verdict: impossible on official firmware.** Neither Flipper can hear the other; this
is enforced at three independent levels, so no app-level workaround exists.

Why:

1. **The radio co-processor stack is slave-only.** Official firmware ships
   `stm32wb5x_BLE_Stack_light_fw.bin` (`fbt_options.py`: `COPRO_STACK_TYPE = "ble_light"`).
   Per ST's release notes, this configuration is *"GAP peripheral only... not able to
   scan and request a BLE connection. It will just advertise, and accept incoming
   connection request from other master devices"*. Scanning, the central role, the GATT
   client, and Direct Test Mode are removed from the binary itself. (This is also why the
   firmware's own `bt_debug` app refuses to run on the stock stack — it checks
   `furi_hal_bt_is_testing_supported()`.)
2. **The firmware initializes GAP as peripheral only.** `targets/f7/ble_glue/gap.c` calls
   `aci_gap_init(GAP_PERIPHERAL_ROLE, ...)`; there is no call to any scan/connect API
   anywhere in the codebase.
3. **No escape hatch is exported to FAPs.** Zero `aci_*` functions in the symbol table,
   `hci_send_req` is marked internal (`-`), and the GAP/HCI command headers are not
   shipped in the SDK at all.

There is no receive path even without a connection: the "extra beacon" API
(`furi_hal_bt_extra_beacon_*`) is transmit-only (31-byte advertising payload), and no API
exists to receive or scan advertisements. Two Flippers can both advertise; neither can
listen. The DTM RX functions (`furi_hal_bt_start_packet_rx`) are removed from the light
stack and would only return packet counts anyway.

What BLE *can* do: a FAP can run a fully custom GATT **server** profile
(`ble_gatt_service_add` / `ble_gatt_characteristic_*` + `bt_profile_start`; the firmware
`hid_ble` app proves the pattern), serial-profile MTU is 414 bytes, throughput tens of
KB/s. The peer must be a central: a phone or PC. A "Flipper ↔ phone-app ↔ Flipper" relay
is therefore technically possible but is a different product (requires a companion mobile
app) and is out of scope for Flipper Share.

What would change the verdict: official firmware switching to the full BLE stack
(`ble_full`) **and** initializing a central-capable GAP **and** exporting scan/connect
APIs — three independent upstream changes. Not worth tracking.

## USB — NOT FEASIBLE

**Verdict: impossible.** Two USB *devices* cannot talk to each other; a USB link needs a
host, and Flipper has none.

Why: the USB stack (`lib/libusb_stm32`) is device-only, and the STM32WB55 has no OTG/host
controller — there is no host code anywhere in the firmware tree. The exported surface
(`furi_hal_usb.h`, `furi_hal_usb_cdc.h`) offers device configs only (CDC single/dual,
HID, U2F, CCID). `furi_hal_power_enable_otg` merely switches the 5 V boost onto GPIO
pin 1 to power expansion modules — it is not USB host.

What would change the verdict: nothing short of new hardware. (A PC in the middle
bridging two Flippers over CDC is trivially possible but is a PC tool, not a
Flipper-to-Flipper transport.)

## Audio (speaker) — NOT FEASIBLE

**Verdict: impossible.** One-way hardware only.

Why: the speaker is a TIM16 PWM square-wave generator (`furi_hal_speaker.h` — frequency +
volume only, no waveform buffer, no DAC). More decisively: **Flipper Zero has no
microphone and no audio ADC input** — the ADC channels (`furi_hal_adc.h`) map only to
GPIO header pins and internal references. A Flipper could chirp FSK at a phone, but
another Flipper can never hear it.

What would change the verdict: an external microphone module on a GPIO ADC pin — at that
point a wire already connects the devices and UART is strictly better.

## Light (LED / display backlight) — NOT FEASIBLE

**Verdict: impossible.** No photosensor.

Why: there is no photodiode, ambient-light sensor, or camera anywhere in the hardware.
The notification LED sits behind an I2C GPIO expander (kHz-class switching at best), and
the only light-sensitive component — the IR receiver — is a demodulating TSOP front end:
hardware band-passed around 38 kHz with AGC, exposing only demodulated edge timings
(`furi_hal_infrared_async_rx_*`). Unmodulated visible light produces nothing at its
output, and constant 38 kHz illumination is suppressed by the AGC. (Light → IR-receiver
tricks are moot anyway: the IR transport already exists.)

What would change the verdict: an external phototransistor on an ADC pin — same wire
objection as above.

## Vibration → motion sensing — NOT FEASIBLE

**Verdict: impossible.** No receiver side.

Why: `furi_hal_vibro_on(bool)` exists, but Flipper Zero has **no accelerometer** (the
only accelerometer references in the firmware tree are unused vendor BSP files and a
Video Game Module test case — an external accessory).

## I2C / SPI on the GPIO header — NOT FEASIBLE (as such)

**Verdict: impossible with the exported HAL.** Master-only on both buses.

Why: `furi_hal_i2c_*` (external bus I2C3, pins 15/16) and `furi_hal_spi_*` (external bus,
pins 2–5) export only master/controller operations. No slave/peripheral mode is exported
for either bus, so two Flippers would be two masters with nobody to talk to.

What would change the verdict: an exported I2C/SPI slave HAL upstream. Even then it would
merely tie the same wires UART already uses — no practical gain over
`flipper_share_uart`. Not worth tracking.

## Raw GPIO bit-banging — FEASIBLE BUT POINTLESS

**Verdict: technically possible, deliberately rejected.**

Why it would work: the SDK exports everything needed for a custom wire PHY —
`furi_hal_gpio_*` incl. EXTI callbacks, `furi_hal_interrupt_set_isr`, `furi_hal_bus_*`,
and all STM32 LL headers (TIM/DMA/EXTI/COMP initializers included). LPTIM2 and DMA1
channels 1–5 are free of firmware owners; MHz-class signaling is achievable.

Why it is rejected: it uses the same jumper wires as USART1 while re-implementing in
software what the USART does in hardware (framing, timing, error detection). Strictly
worse effort-to-result than `flipper_share_uart` with zero UX difference. The only niche
— a 2-wire link — is better served by the USART's own single-wire half-duplex mode (a
`flipper_share_uart` v2 idea).

Timer ownership map, for reference if a custom PHY is ever needed: TIM1 = RFID carrier /
iButton emulate / IR TX; TIM2 = RFID emulate+capture / IR RX / Sub-GHz; TIM16 = speaker;
TIM17 = NFC; LPTIM1 = system tick (do not touch); LPTIM2 = free.

## SD card ("sneakernet") — FEASIBLE, NOT A TRANSPORT

**Verdict: works today, nothing to build.** Physically moving the microSD card (or
copying through a PC/phone via qFlipper) transfers any file with perfect reliability.
Listed only for completeness — it needs no app, so it is not a Flipper Share transport.

---

## Summary

| Candidate | Verdict | Blocking fact |
|---|---|---|
| BLE | impossible | slave-only copro stack: no scan, no central, no receive path; no HCI/ACI export |
| USB | impossible | device-only stack; STM32WB55 has no host controller |
| Audio | impossible | no microphone, no audio ADC |
| Light | impossible | no photosensor; IR RX is a 38 kHz demodulator |
| Vibration | impossible | no accelerometer |
| I2C / SPI | impossible | master-only HAL, no slave mode exported |
| Raw GPIO PHY | possible, rejected | same wires as UART, all-software PHY, no benefit |
| SD card | trivial, out of scope | not an app |

Bottom line: after UART, iButton, and LF RFID, the practically available transport list
for Flipper-to-Flipper on official firmware is **exhausted**.
