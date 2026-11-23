# BCM4773 sensor hub (SSP over BBD) on Note 4: feasibility notes

Source: universal5433/android_kernel_samsung_universal5433 @ lineage-18.1, drivers/sensorhub/brcm/
(a local copy was fetched into the session scratchpad; line numbers below refer to that tree).
Device tree: universal5433/android_device_samsung_trelte-common @ lineage-18.1.

## 1. Boot flow (who does what)

Init order:
- `bbdpl/bbd_main.c:359` `subsys_initcall(bbd_device_init)` creates /dev/bbd_{sensor,control,packet,reliable,patch,ssi_spi_debug}
  (major 239, bbd_main.c:221-227), registers platform dev "bbd" and sysfs, then calls `bbd_ssi_spi_open()`
  (bbd_main.c:290). That function is `bbd_tty_open()` (bbd_ssi_spi.c:53-57): the kernel does `filp_open("/dev/ttyBCM0")`
  from inside the kernel (bbd_tty.c:80, 381) and pushes the bytes it reads into `BbdBridge_SetData` (bbd_tty.c:354).
- `bbdpl/bcm_gps_spi.c:1972` `module_init` → `spi_register_driver`, compatible "ssp,BCM4773" (bcm_gps_spi.c:1804).
  `bcm4773_spi_probe` (1350):
  - takes ssp-irq (HOST_REQ) (1379)
  - resets the FIFO (1490) and identifies the chip (1494)
  - registers a fake UART, **/dev/ttyBCM0** (1513-1531)
  - reads mcu-req/mcu-resp from DT (1538-1539)
  - **calls `pssp_driver->probe(spi)` (1556)**, which is the SSP driver's `ssp_probe`, sharing the same spi_device
  - requests the IRQ (1559)
- `ssp_dev.c:261` `ssp_probe`: input/sysfs/iio setup, then `bbd_register(data, &ssp_bbd_callbacks)` (ssp_dev.c:378).
  It does **not** talk to the MCU at probe time.
- MCU init happens only in `callback_bbd_on_mcu_ready` (ssp_bbd.c:175), which queues `bbd_mcu_ready_work_func`
  (ssp_bbd.c:228 → `initialize_mcu`, ssp_dev.c:142: WHOAMI==0x55, sensor position, scanning info, mag init, fw rev).
- `on_mcu_ready` is called **only** from `bbd_ESW_control()` when the string "ESW:READY" arrives (bbd_control.c:200-207).
  That string comes from `bbd_control_write()`, i.e. **userspace LHD writing /dev/bbd_control** (bbd_control.c:166-174),
  or from the sysfs debug file /sys/devices/platform/bbd/ESW (bbd_sysfs.c:57-75).

Power and reset:
- The BCM4773 nSTANDBY/PWR_EN pin is gpg2-0 in the DT "gps" node (compatible "samsung,exynos54xx-bcm4753",
  pins &gpg2 0 and host_wake &gpa1 0; tre_eur_open_16.dts:336-345).
- `drivers/gps/sec_gps_bcm47531.c:254-256` drives it **low** and `gpio_export`s it as /sys/class/sec/gps/GPS_PWR_EN.
  The kernel never raises it.
- trelte-common `init.universal5433.rc:514-515` chowns GPS_PWR_EN/value to the system group, so userspace (LHD) toggles it.

Firmware (patch):
- `bbd_patch.c:25-34` embeds `bbd_patch_file.h` as a char array, and that array is used **only** by
  `bbd_patch_read()` (bbd_patch.c:65-83), which serves /dev/bbd_patch to userspace.
- No kernel code downloads it.
- The header (bbd_patch_file.h:1-12) is a text container: `<FormatVersion=0x00010001> <Crc=172> <BlobLength=296935>
  <AsicVersion=0x47730A20> <Name=bcm_sensorhub_a2> <Type=Patch> <PatchAddress=0x00824401> <Blob=hex...>`.
  The blob is about 290 KiB of binary.
- The same file also ships as /vendor/bin/bcm_sensorhub_a2.patch (trelte-common proprietary-files.txt).
- Kernel paths LHD could use for the download:
  - /dev/bbd_ssi_spi_debug: SSI "debug mode" DMA writes and reads of the chip's memory at an arbitrary address
    (bbd_ssi_spi.c:164-426; commands 0x80 0xD1 0x00 addr32 len16 0x03, then 0x80 0xD1 0x24 data).
    The comment says "push download data directly into 477x memory, bypassing all TL, RPCs, CRC" (bbd_ssi_spi.c:70-76).
  - /dev/bbd_packet in pass-through mode. The transport layer starts in pass-through
    (transport_layer_c.c:239 `m_bPassThrough = true`), and in that mode bytes from the chip go straight to LHD
    (bbd_bridge_c.c:317-320).
- LHD then sends the control strings "BBD:PassThru=0" and "PL:StartRemoteSync" (bbd_packet_layer_c.c:36-55), followed
  by "ESW:READY".
- **The exact patch-download and start sequence lives only in the closed LHD binary.**

Userspace (trelte-common lineage-18.1):
- `init.universal5433.rc:799` `service lhd /system/vendor/bin/lhd /system/vendor/etc/lhd.conf` (user gps)
- `:826` `service gpsd /vendor/bin/gpsd -c /vendor/etc/gps.xml` (gps.xml `PortName="lhd"`)
- `init.samsung.rc:764` sensorhubservice (Samsung extras only)
- ueventd: /dev/bbd_*, /dev/ttyBCM*, iio:device*

**Conclusion:** stock SSP needs LHD for power-up, the patch download, the transport switch-over and the ESW:READY
handshake. After ESW:READY, the sensor data path runs entirely in the kernel (shmd=true, bbd_main.c:199):

    ssp → bbd_send_packet → BbdEngine_SendSensorData (bbd_sensor.c:112-115)
        → TransportLayer → BbdTransportCB_OnDataToSend_TTY → ttyBCM0 → SSI-SPI

    SSI-SPI → ttyBCM0 → BbdBridge_SetData → TransportLayer
        → RPC IRpcSensorResponse.Data → SensorRpc_ProcessRpc (bbd_engine.c:206-217)
        → bbd_sensor_write_internal → on_packet_alarm
        → bbd_on_packet_work_func → parse_dataframe (ssp_bbd.c:275-418)

GPS packets go to LHD through /dev/bbd_packet.

## 2. Minimal pieces for sensors only (LOC as in the downstream tree)

| Layer | Downstream files (LOC) | Needed for kernel-only |
|---|---|---|
| Power | sec_gps_bcm47531.c (~340, export only) | gpiod for gpg2-0 (~20) |
| SSI-SPI physical: HOST_REQ IRQ, MCU_REQ/RESP handshake, cmd_stat byte, half/full duplex, FIFO status | bcm_gps_spi.c 1977 (core about 600: hello/bye 730-790, transfer 808-930, read_transmit/write_receive 950-1005, irq + rxtx_work 1006-1240, autobaud/identify 1242-1340) | rewrite about 400 with no fake UART/tty |
| Kernel tty plumbing | bbd_tty.c 474, bbd_sio.c 165 | drop, call the bridge directly |
| Patch download (ROM bootloader or SSI debug DMA) | not in the kernel, only in LHD | **unknown**, needs RE; probably 200-400 |
| Transport layer (framing, seq/ack, CRC, reliable, remote sync) | transport_layer_c.c 1696, bbd_packet_layer_c.c 126, bbd_bridge_c.c 333, bbd_engine.c 443, utils (crc8 86, ring_buffer 158, stream_codec 291), rpc_codec_c.h 83 | reuse mostly as is (portable C), about 2.5-3k |
| BBD char devices / control | bbd_common 538, control 289, sensor 345, packet 146, reliable 157, sysfs 326, patch 83 | drop, about 100 lines of glue |
| SSP protocol: msg {cmd,len,options,data} 9-byte header (ssp.h:367), pending list, AP2HUB_READ/WRITE, HUB2AP_WRITE | ssp_bbd.c 418, ssp_i2c.c 758 | about 400 |
| SSP commands: WHOAMI 0x0F→0x55, BYPASS_SENSOR_ADD 0xA1, REMOVE 0xA2, CHANGE_DELAY 0xA4, SENSOR_SCANNING 0xF4, set position | ssp.h:146-176, ssp_i2c.c | in mainline already |
| Data parsing: MSG2AP_INST_BYPASS_DATA 0x37, 3-axis s16 + timestamp | ssp_data.c 381 | about 150 for accel |
| IIO | ssp_iio_ring.c 84, ssp_input.c 1235, ssp_sysfs.c 1030 | reuse mainline |

**Mainline already has the SSP command set and IIO front end:**
drivers/iio/common/ssp_sensors/ (ssp_dev.c 668, ssp_spi.c 601, ssp_iio.c 97, ssp.h 246;
compatibles "samsung,sensorhub-rinato" and "samsung,sensorhub-thermostat", ssp_dev.c:430-433), plus
drivers/iio/accel/ssp_accel_sensor.c (144) and drivers/iio/gyro/ssp_gyro_sensor.c (144).
- It was written by Karol Wrona (Samsung) for an STM32 hub that sits directly on SPI with the same command values
  (SSP_DEVICE_ID 0x55, 0xa1, 0xa4, WHOAMI 0x0f).
- Its transport (ssp_spi.c, GPIO handshake plus raw spi_write) has to be replaced by a "BBD transport".
  The idea is to factor ssp_spi.c into ops and add an ops implementation that uses TL+SSI.

Total new or ported code for accel only: about 3.5-4.5k LOC, most of it the ported Broadcom transport.

## 3. Licensing and blobs

- All kernel C is GPL: Broadcom bbdpl is "Dual BSD/GPL" (bbd_main.c:357) or GPLv2 (bbd_patch.c:1-16), and the
  Samsung ssp files are GPLv2+.
- **bcm_sensorhub_a2 patch is an opaque 296,935-byte firmware blob** (code for the BCM4773's own CPU at 0x00824401).
  - It is published in the GPL kernel tree as hex, but there is no source. Treat it as linux-firmware-style:
    extract it to /lib/firmware/bcm4773_sensorhub_a2.patch and load it with request_firmware.
  - The same file is in /vendor/bin on the phone. A mismatch between the patch and the ROM revision
    (AsicVersion 0x47730A20) is possible on other boards, so use the file from the phone.
- Closed userspace: lhd (download/start sequence, GPS), gpsd/libgps (GPS only), sensorhubservice/libsensorhub
  (Samsung extras, not needed).
- For sensors only, **LHD is the sole closed dependency, and only for its boot procedure**.

## 4. Existing work

- Mainline: drivers/iio/common/ssp_sensors, merged in 2015 (series v1-v5 by Karol Wrona, lore.kernel.org 54CA81A5.50206@kernel.org).
  It covers the STM32 hub only. Nothing covers BBD/BCM477x.
- Searches turned up nothing on any of these:
  - a mainline or postmarketOS driver for the BCM4773 sensor hub, an LHD reimplementation, or the strings
    "ESW:READY", "bcm_sensorhub_a2" or "bbd_ssi_spi_debug" outside vendor trees
  - pmOS pages for klte/zeroflte/trelte that report working sensors
- The pmOS wiki page for trelte now redirects off-site, so it was not followed.
- Google gs101 (Pixel 6, BCM47765) ships an lhd.conf with `/dev/bbd_control` and `LhePatch=`. That confirms the same LHD
  architecture is still in use and still closed.

## 5. Options and risks

A. **Hybrid / quick hack**: port bbdpl + ssp to 7.3 out of tree, then run the stock bionic `lhd` (from the phone's
   /system) under the Android linker or libhybris on pmOS.
   - The port has to deal with wake_lock, timespec, filp_open of a tty from the kernel, s3c_gpio, and old uart_ops.
   - LHD wants Android properties and liblog.
   - Estimate 5-10 days.
   - Ugly but deterministic. Also useful as the **reference and trace source** for B: run it with BBD/SSI debug
     enabled and log every SPI transfer.

B. **Kernel-only (clean)**: new driver = gpiod power + SSI-SPI + request_firmware patch download + ported TL/RPC
   + mainline ssp_sensors with a BBD backend.
   - 1. RE of LHD's download/handshake: Ghidra on lhd, or traces from A (or from stock Android if it can still boot).
        3-10 days, the main unknown.
   - 2. SSI-SPI + TL port: 4-6 days.
   - 3. SSP glue + accel IIO + DT + mount matrix (ssp-acc-position=5): 2-3 days.
   - Total about 2-4 weeks for accel → iio-sensor-proxy. Gyro, mag (YAS532 calibration matrix), light/prox and pressure
     are then cheap (+1-3 days).

Risks:
- The download protocol is undocumented (ROM bootloader commands, patch CRC/format, jump/start, autobaud).
- HOST_REQ and MCU_REQ/RESP timing quirks (many FIXMEs in bcm_gps_spi.c; IRQF_TRIGGER_HIGH level IRQ).
- The Exynos5433 SPI0 controller in mainline (spi-s3c64xx, cs-gpio gpd8-1, mode 3, 26 MHz) and DMA.
- The TL's 'remote sync' and timer-driven retransmit need a tick (BbdEngine_Tick).
- Blob redistribution.
- The hub may require periodic time sync (MSG2AP_INST_TIME_SYNC / set_time) or watchdog resets.
- GPS cannot be had without the closed LHD+gpsd in either case.
