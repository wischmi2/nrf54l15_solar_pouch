# nRF54L15 solar board bring-up

This is the custom solar pouch board: an nRF54L15 plus an nPM1300, programmed from an nRF9160 DK used as an external SWD debugger. The DK is the programmer. It is not the bootloader. The bootloader on the nRF54L15 is MCUboot.

The SDK is nRF Connect SDK v3.4.0 at `C:\ncs\pouch-ncs`. The build target is `nrf54l15dk/nrf54l15/cpuapp`. That DK name is only the SoC base. The image is for this board because of the overlay in `boards/nrf54l15dk_nrf54l15_cpuapp.overlay`.

The J-Link on the DK is USB serial `960059301`. Use J-Link V9.82 (`C:\Program Files\SEGGER\JLink_V982`). V9.36 drops the debug link when the nRF54L15 goes to sleep.

## Hardware the firmware had to match

The board has no 32.768 kHz crystal. The overlay disables `&lfxo`, and the clock source is the internal LF RC (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC` and `CONFIG_NRF_GRTC_TIMER_SOURCE_LFLPRC` on MCUboot). Leaving the DK's LFXO enabled makes GRTC wait forever and `main` never runs.

The nPM1300 is on I2C at address `0x6B`. SCL is P1.03, which is an NFC pin, so UICR `nfct-pins-as-gpios` is required. SDA is P1.04. The interrupt is P0.00. Antenna select is P0.01, held high for the external antenna connector. BUCK2 is 3.0 V. Set the DK voltage switch (SW9) to 3.0 V so the debug probe matches the board. The board is self-powered. The DK's debug-out header (P19) is the SWD connection. The DK UART pins are the PMIC I2C bus and the interrupt, so the firmware has no serial console.

The nPM1300 LED driver sinks about 5 mA when a pin is on. Charging mode lights a pin whenever solar is present, and mode "not used" does not turn the pin off. The firmware sets all three LEDs to host mode and then writes the CLR register. The PMIC does not reset when the nRF resets, so a latched LED stays on until the board is unplugged and plugged back in.

## Build

`west flash` does not work on this Windows machine. The pouch runner imports `pty` and `termios`, which are not available, and the build dies with `ModuleNotFoundError`. Build with `west`, then program with `nrfutil`.

From the project directory:

```powershell
nrfutil sdk-manager toolchain launch --ncs-version v3.4.0 --chdir C:\ncs\pouch-ncs -- west build -b nrf54l15dk/nrf54l15/cpuapp -d C:\Users\Brian\pouch_nrf54l15_v3\build C:\Users\Brian\pouch_nrf54l15_v3
```

The images that get programmed are:

- `build\mcuboot\zephyr\zephyr.hex` at RRAM address `0x0`
- `build\pouch_nrf54l15_v3\zephyr\zephyr.signed.hex` at `0x10000`

There is no `merged.hex`. Program MCUboot first, then the signed application.

## Programming

Omitting `chip_erase_mode` on an nRF54L makes `nrfutil` erase the whole chip. Always pass `ERASE_NONE` unless a full erase is intentional. A full erase, and `recover`, wipe the credential filesystem.

```powershell
nrfutil device program --serial-number 960059301 --firmware C:\Users\Brian\pouch_nrf54l15_v3\build\mcuboot\zephyr\zephyr.hex --options chip_erase_mode=ERASE_NONE,verify=VERIFY_READ

nrfutil device program --serial-number 960059301 --firmware C:\Users\Brian\pouch_nrf54l15_v3\build\pouch_nrf54l15_v3\zephyr\zephyr.signed.hex --options chip_erase_mode=ERASE_NONE,verify=VERIFY_READ,reset=RESET_SYSTEM
```

Close J-Link RTT Viewer first. It owns the probe, and the program command cannot attach while the viewer is open.

## MCUboot was linked into the application slot

The first images booted the wrong thing, so RTT stayed empty.

`sysbuild/mcuboot.overlay` replaces MCUboot's own `app.overlay`. That upstream file is the one that sets `zephyr,code-partition` to `boot_partition` at address `0x0`. The board devicetree points `zephyr,code-partition` at `slot0_partition` (`0x10000`), which is correct for the application and wrong for the bootloader. Without the chosen node in our overlay, MCUboot was linked at `0x10000`. Programming the signed app then overwrote it. The CPU resets from `0x0`, so it never reached the application, and the application is what creates the RTT block.

The overlay now starts with:

```dts
/ {
	chosen {
		zephyr,code-partition = &boot_partition;
	};
};
```

After that fix, MCUboot occupies the 62 KB boot partition at `0x0` (about 39 KB used). The signed application stays at `0x10000`, with its vector table at `0x10800`.

## The access port locked

One flash with `ERASE_NONE` came back as `The Application core access port is protected (All)`. `nrfutil device device-info` showed `NRFDL_PROTECTION_STATUS_ALL` and an unreadable `nRF54L15_xxxx_REV2`. The firmware is built with `CONFIG_NRF_APPROTECT_DISABLE=y` and `CONFIG_NRF_SECURE_APPROTECT_DISABLE=y`. The disable value lives in UICR, which is write-once, so a program that does not erase UICR can leave the port locked after the next boot.

Recovery was required. It erases all user NVM, including the LittleFS credential partition, and it clears access-port protection.

```powershell
nrfutil device recover --serial-number 960059301
```

After it succeeds, device info returns `NRFDL_PROTECTION_STATUS_NONE` and `nRF54L15_xxAA_REV2`. Program MCUboot and the signed app again. Do not recover once certificates are on the chip unless the port is locked again.

## J-Link and RTT

RTT is the only log channel. The shell is off. Its 10 ms poll holds the CPU out of idle. There is no UART console, so the text box at the bottom of RTT Viewer has nothing to talk to. Typing there produces `Failed to write input to target` and `Sent 0 of 47 bytes`. That is expected. The log lines above that error are the chip talking.

Open `C:\Program Files\SEGGER\JLink_V982\JLinkRTTViewer.exe` and set:

| Field | Value |
| --- | --- |
| Connection | USB, serial `960059301` |
| Device | `nRF54L15_M33` |
| Interface | SWD |
| Speed | `4000` kHz |
| RTT control block | Address `20002070` |

Leave Auto Detection off. Connect, then reset the board if you want the boot lines from the start. The up buffer is 8 KB (`CONFIG_SEGGER_RTT_BUFFER_SIZE_UP=8192`). Logs are in drop mode, so a viewer attached long after boot can miss the oldest lines once the buffer wraps. The block sits at `0x20002070` because 0x60 bytes of KMU-reserved RAM sit at `0x20000000` and the 8 KB up buffer is placed before the control block. An earlier build with a 1 KB buffer had the block at `0x20000470`. Auto search misses it. If the buffer size changes, look up `_SEGGER_RTT` in `build\pouch_nrf54l15_v3\zephyr\zephyr.map` before trusting the address.

A command-line capture, which is what was used to confirm the block while the viewer was pointed at the old address:

```powershell
& "C:\Program Files\SEGGER\JLink_V982\JLinkRTTLogger.exe" -USB 960059301 -Device nRF54L15_M33 -If SWD -Speed 4000 -RTTAddress 0x20002070 -RTTChannel 0 C:\Users\Brian\pouch_nrf54l15_v3\rtt.log
```

To check that the block is actually in RAM, without the viewer:

```powershell
nrfutil device read --serial-number 960059301 --address 0x20002070 --bytes 16 --direct
```

The first bytes are the ASCII magic `SEGGER RTT`.

### Reset

This resets the nRF54L15 on the debug cable. It does not reset the nRF9160. Serial `960059301` is the J-Link chip on the DK. The DK stays powered and keeps running as the debugger.

```powershell
nrfutil device reset --serial-number 960059301
```

Run it only when RTT Viewer is closed. The viewer already owns the probe, so the command either fails as busy or steals the probe and the viewer stops updating. The USB device itself stays enumerated. After the reset, open the viewer again at `20002070`. The boot text is still in the RTT buffer.

Unplugging the board is a different operation. That power-cycles the nPM1300 as well as the nRF. A debugger reset does not.

## Certificates

Pouch reads two DER files from LittleFS at boot:

- `/lfs1/credentials/crt.der`
- `/lfs1/credentials/key.der`

`/lfs1` is the mount point. Inside the filesystem image the directory is `/credentials`. The running firmware cannot receive these files. There is no UART for the example's `mcumgr` commands, and the shell and MCUmgr are turned off. The RTT input box is not a file transfer.

The files used here were already DER, dropped in the project folder:

- `nrf54l15_v3_solar.crt.der` (381 bytes)
- `nrf54l15_v3_solar.key.der` (138 bytes, P-256 / prime256v1)

If Golioth gives PEM instead, convert first. Do not print the key while doing this.

```powershell
openssl x509 -inform PEM -outform DER -in device.crt.pem -out crt.der
openssl pkcs8 -topk8 -nocrypt -inform PEM -outform DER -in device.key.pem -out key.der
```

`openssl x509 -inform DER -in nrf54l15_v3_solar.crt.der -noout -subject -dates` showed subject `CN=nrf54l15_v3_solar`, issued by `nrf54l15_v3_solar-CA`, valid from 9 Oct 2026 through 6 Nov 2026. `openssl pkey -inform DER -in nrf54l15_v3_solar.key.der -noout -text` showed a 256-bit `prime256v1` key. The file `goliothrootx1.der` is the Golioth root CA. It is already compiled into the firmware and is not the device credential.

### Where the filesystem lives

LittleFS is the storage partition at RRAM `0x174000`, size `0x9000` (nine 4 KB blocks). The geometry has to match `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` and the log line the chip prints at boot (`read 16, prog 16, cache 64, lookahead 32`, block cycles 512). The on-disk format is LittleFS 2.11, disk version 2.1, which is what NCS v3.4.0 mounts. `littlefs-python` 0.19.0 bundles that same library.

The image was built with Python, not by the running firmware:

```python
from littlefs import LittleFS, UserContext

block_size = 4096
block_count = 9
ctx = UserContext(buffsize=block_size * block_count)
fs = LittleFS(
    context=ctx,
    block_size=block_size,
    block_count=block_count,
    read_size=16,
    prog_size=16,
    cache_size=64,
    lookahead_size=32,
    block_cycles=512,
    name_max=255,
    file_max=2147483647,
    attr_max=1022,
)
fs.mkdir("/credentials")
for src, dst in (
    (r"nrf54l15_v3_solar.crt.der", "/credentials/crt.der"),
    (r"nrf54l15_v3_solar.key.der", "/credentials/key.der"),
):
    data = open(src, "rb").read()
    with fs.open(dst, "wb") as fh:
        fh.write(data)
fs.unmount()
image = bytes(ctx.buffer)  # 36864 bytes
```

That buffer was written out as an Intel hex file loaded at `0x174000` (extended linear address `0x0017`, data records starting at offset `0x4000`). The hex was not left in the tree. It contains the private key.

Only that partition was erased, then the hex was programmed with no chip erase, and the nRF was reset:

```powershell
nrfutil device erase --serial-number 960059301 --pages 0x174000-0x17D000

nrfutil device program --serial-number 960059301 --firmware C:\Users\Brian\AppData\Local\Temp\pouch-creds.hex --options chip_erase_mode=ERASE_NONE,verify=VERIFY_READ,reset=RESET_SYSTEM
```

`--pages 0x174000-0x17D000` erases the nine 4 KB pages and stops before `0x17D000`. Close RTT Viewer before either command.

A read of `0x174000` then showed the LittleFS superblock magic `littlefs`, disk version `0x00020001`, and block count `9`. After reset, RTT showed:

```text
<inf> main: Credentials loaded
<inf> main: Pouch initialized
<inf> example_ble_peripheral: Advertising, requesting a gateway
```

`Failed to load certificate (err -2)` means the files are missing (`-2` is "no such file"). That was the log before this partition was programmed.

Do not commit `nrf54l15_v3_solar.key.der` or `nrf54l15_v3_solar.crt.der`. Another `nrfutil device recover`, or a program that chip-erases, deletes them from the nRF and they have to be written again.

## What the firmware is doing right now

The image on the board is a debug build, not the low-power one. It advertises every 100–150 ms with the gateway-request flag left on, and it starts advertising again as soon as a connection drops. `main` prints `Up, advertising for a gateway` every 10 seconds so the RTT window keeps moving. A missing certificate no longer skips advertising. This draws more than the panel can harvest (the panel is about 0.1 mA). The earlier power settings, a 30 second advertise window and then 10 minutes idle, are still described by `EXAMPLE_ADV_WINDOW_S` and `EXAMPLE_SYNC_PERIOD_S` in `Kconfig`, but the debug path in `src/ble_peripheral.c` does not use them.
