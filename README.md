# nRF54L15 solar pouch

Firmware for the custom solar board: an nRF54L15 and an nPM1300, talking to Golioth through a Pouch Bluetooth gateway. The Zephyr board name is `nrf54l15dk/nrf54l15/cpuapp`. That name is only the SoC base. `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` is what matches this board.

The detailed bring-up record, including the J-Link commands and how the device certificate was written, is in [docs/board-bringup.md](docs/board-bringup.md).

The SDK is nRF Connect SDK v3.4.0. The solar board is programmed through an nRF9160 DK used as the SWD debugger (J-Link serial `960054289`). That DK is not the bootloader. MCUboot runs on the nRF54L15. A second nRF9160 DK (J-Link serial `960059301`) is the gateway for another Pouch client. Do not program, reset, or recover that probe.

## Current status

The image programmed on 17 Oct 2026, on J-Link `960054289` with `ERASE_NONE` and a read-back verify, is this tree. RTT works in J-Link V9.82 with the control block at `0x20002070`. The device certificate for `nrf54l15_v3_solar` is still in the LittleFS storage partition.

Each uplink is one JSON object from the nPM1300 snapshot, path `.s/sensor`:

`vbat_mv`, `soc_pct`, `ibat_ma`, `charge`, `vbus`, `usb_cc`, `source`, `aem`, `aem_window`, `vbat_rise_mv`.

The board asks a gateway to connect for 30 seconds (`CONFIG_EXAMPLE_ADV_WINDOW_S`), then clears the sync-request flag for 120 seconds (`CONFIG_EXAMPLE_SYNC_PERIOD_S`). The first window opens at boot. After a session ends, the next request is another 120 seconds later. While the flag is clear, the gateway skips this board and can serve another one. Advertising stays at 100–150 ms in both phases, and `main` still prints a line every 10 seconds, so this draws more than the panel can harvest. The panel is about 0.1 mA.

No gateway session has completed. The nRF9160 DK gateway gets as far as the modem PPP link, then fails to finish DTLS to `coap.golioth.io:5684` (`-113`) and later fails the DNS lookup (`-101`).

## Changelog

### 17 Oct 2026 — share the gateway

- Request a gateway for 30 seconds, then leave the sync-request flag clear for 2 minutes so another Pouch board can be chosen.
- Heartbeat reports `requesting a gateway` or `gateway request is off`.
- Built and flashed to the solar board on J-Link `960054289`. Credentials were left in place.

### 17 Oct 2026 — bring-up record and debug radio

- Added this README and `docs/board-bringup.md`.
- Replaced the fixed `{"temp":22}` uplink with the nPM1300 snapshot in `src/solar_status.c`.
- Left the radio advertising continuously, with the gateway-request flag held on, so a session could be watched on RTT. That image is what kept the other board from connecting.

### 8 Oct 2026 — initial firmware

- nRF54L15 application with MCUboot, Pouch over BLE GATT, device credentials read from LittleFS, and the nPM1300 board overlay.

## What has been done

- The nPM1300 is on I2C at `0x6B` (SCL P1.03, SDA P1.04, interrupt P0.00). Antenna select P0.01 is held high. There is no 32 kHz crystal, so the internal LF RC is used. The DK UART pins are disabled.
- The nPM1300 LEDs are forced off. They only stay off across an nRF reset if the mode is host and CLR is written. A latched LED needs a power cycle of the PMIC.
- MCUboot is linked at RRAM `0x0`. An overlay bug had linked it into the application slot, so the application never started and RTT stayed empty.
- The access port was recovered once after it locked. Recover erases the credential filesystem. `CONFIG_NRF_APPROTECT_DISABLE` is set so later flashes with `ERASE_NONE` can leave the port open.
- Programming is `nrfutil device program` with `chip_erase_mode=ERASE_NONE`. `west flash` fails on this Windows machine because the pouch runner imports `pty`.
- Logs are SEGGER RTT only. The shell is off. The certificate and key were written by building a LittleFS image and programming it at `0x174000`, not by a serial upload.
- `src/solar_status.c` samples the nPM1300 charger on its own thread and classifies solar harvest from the AEM10330 thresholds (charge to 4.05 V, ready 3.50 V, cut-off 3.00 V).

## Gateway

The gateway is not this firmware. On the nRF9160 DK it is two images: the gateway on the nRF52840 and the serial modem on the nRF9160. The solar board's RTT stays on its own debugger (`960054289`). A completed session would show the uplink in Golioth on the `nrf54l15_v3_solar` device. The DK's own cloud connection is not up yet.

## Building

```powershell
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir C:\ncs\pouch-ncs -- west build -b nrf54l15dk/nrf54l15/cpuapp -d C:\Users\Brian\pouch_nrf54l15_v3\build C:\Users\Brian\pouch_nrf54l15_v3
```

Close J-Link RTT Viewer before programming. Program only serial `960054289`. The certificate files `*.crt.der` and `*.key.der` are gitignored. Do not commit them, and do not chip-erase or recover the board if those files should stay on the device.
