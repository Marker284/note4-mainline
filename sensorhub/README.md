# Note 4 (Exynos) BCM4773 sensor hub: out-of-tree port to Linux 7.3

This is a port of the downstream Broadcom BBD (`drivers/sensorhub/brcm/bbdpl`) and the
Samsung SSP code (`drivers/sensorhub/brcm/ssp_*`) from universal5433 lineage-18.1.
The pristine downstream copy is in `../ref/sensorhub-ds/`, and the background is in
`../ref/sensorhub-notes.md`.

Build:

    make -C ../build/kernel ARCH=arm LLVM=1 LOCALVERSION= M=$PWD modules
    # or simply: make

The kernel tree has no `Module.symvers` because only vmlinux was built. Because of that,
`Makefile` sets `KBUILD_MODPOST_WARN=1`, and modpost reports unresolved kernel/IIO
symbols as warnings.

## Modules and load order

| module | contents | binds to |
|---|---|---|
| `bbd.ko` | bbdpl core, char devices, sysfs, transport layer and utils, SSI-SPI driver (`bcm_gps_spi.c`), `/dev/ttyBCM0` | SPI device with `compatible = "ssp,BCM4773"` |
| `ssp_bbd.ko` | SSP protocol (`ssp_bbd_link.c` = downstream `ssp_bbd.c`, renamed because a member object cannot share the module's name; `ssp_i2c.c`, `ssp_data.c`), enable/delay logic, IIO | platform device `bbd-ssp`, which `bbd.ko` creates as a child of the SPI device |

1. Load `bbd.ko` first. It creates `/dev/bbd_*` and then registers the SPI driver.
   When the SPI driver probes, it creates the `bbd-ssp` child device.
2. Load `ssp_bbd.ko` second. It uses symbols from bbd.ko, so modprobe pulls bbd.ko in
   automatically. Both modules have modaliases (`of:...ssp,BCM4773` and
   `platform:bbd-ssp`).
   - It also needs `industrialio.ko` (CONFIG_IIO=m). Without a kernel Module.symvers,
     modpost cannot record that dependency, so ssp_dev.c declares
     `MODULE_SOFTDEP("pre: industrialio")`.
   - The kernel package must ship industrialio.ko, or the kernel needs CONFIG_IIO=y.
3. Start LHD. It powers the chip, downloads the patch (`/dev/bbd_patch` read →
   `/dev/bbd_ssi_spi_debug` or `/dev/bbd_packet`), sends `BBD:PassThru=0` and
   `PL:StartRemoteSync`, and then sends `ESW:READY` on `/dev/bbd_control`.
4. On `ESW:READY`, ssp_bbd waits 1 s and then initialises the MCU: WHOAMI 0x55, sensor
   positions, scanning info, mag matrix and firmware revision. The IIO raw reads only
   start working after this. Check `/sys/bus/platform/devices/bbd-ssp/ready`.

Debug without LHD: `echo ESW:READY > /sys/devices/platform/bbd/DEV` fakes the handshake.
This is only useful once the ESW is actually running.

## Userspace ABI kept (for the closed `lhd`)

- Char devices keep major 239 (the `major=` parameter can change it; the default falls back
  to a dynamic major if 239 is busy), the same minors 0..5, and the same names.
  There is a class "bbd".
- Read/write/poll semantics, control strings (`bbd_ifc.h`),
  `struct sBbdReliableTransaction` and `struct bbd_download_direct` (12 bytes on arm32)
  are unchanged.
- `/sys/devices/platform/bbd/{BBD,ESW,DEV,debug,baud,shmd,pl,buf,passthru,ssi_*}` is kept.
  `debug` and `baud` are now 0664 because sysfs refuses world-writable files.
- `/dev/bbd_patch` serves exactly the downstream bytes, but they now come from
  `request_firmware("bcm4773/bcm_sensorhub_a2.patch")` (module parameter `patch_fw`).
  - A ready-made copy is at `out/bcm4773/bcm_sensorhub_a2.patch`; install it as
    `/lib/firmware/bcm4773/bcm_sensorhub_a2.patch`.
  - Create the file with `tools/extract_bbd_patch.py ../ref/sensorhub-ds/bbdpl/bbd_patch_file.h bcm_sensorhub_a2.patch`.
    The output is 606992 bytes, including the trailing NUL that downstream's `sizeof()`
    counted.
  - Alternatively, build with `BBD_EMBED_PATCH=/abs/path/bbd_patch_file.h` to compile it
    in as downstream did.
- `/dev/ttyBCM0` (bbd.ko parameter `tty=1`) is a plain tty_driver. While userspace has it
  open, received bytes go to the tty instead of the bridge.
- `/sys/class/sec/gps/GPS_PWR_EN/value` exists if the node has `enable-gpios` (or
  `gps-pwr-en-gpios`). It emulates downstream `gpio_export_link()`, which LHD's init
  scripts chown.

## DT properties (on the `ssp,BCM4773` SPI node; see `treltexx-sensorhub.dtsi`)

bbd.ko:
- `host-req-gpios` (HOST_REQ gpa3-3, **required**).
  - Requested as-is, so it is never re-muxed out of EINT.
  - The IRQ is the node's first `interrupts` entry (level high); without one the driver
    uses `gpiod_to_irq(host-req)`.
- `mcu-req-gpios` (gpf2-3) and `mcu-resp-gpios` (gpf2-2) are optional.
  - They drive the MCU_REQ/RESP handshake; without them the handshake is skipped.
- `enable-gpios` (gpg2-0, GPS_PWR_EN/nSTANDBY) is optional.
  - The driver keeps the current level and only forces it low if the pin was an input.
- Downstream-style names `ssp-irq-gpios`, `ssp-mcu-req-gpios`, `ssp-mcu-resp-gpios` and
  `gps-pwr-en-gpios` are also accepted.
- `ssp-hw-rev` is read but unused.
- `spi-cpol` and `spi-cpha` are set; the driver forces SPI mode 3 anyway.

ssp_bbd.ko (same node):
- `ssp-acc-position` (5) and `ssp-mag-position` (3) are sent to the hub. The hub rotates
  the data into the Android frame.
- `ssp,prox-hi_thresh` and `ssp,prox-low_thresh` (130/90).
- `ssp-mag-array` (YAS532 soft-iron matrix, 9 cells).
- `ssp-ap-rev`.
- Optional `accel-mount-matrix`, `gyro-mount-matrix` (falls back to the accel one),
  `mag-mount-matrix` / `magn-mount-matrix`, and `mount-matrix`. The default is identity.

## IIO devices (all under the bbd-ssp device)

| name | attributes | hub sensor / scale |
|---|---|---|
| ssp-accel | `in_accel_{x,y,z}_raw`, `_scale`, `_sampling_frequency`, `in_accel_mount_matrix` | type 0; 9.80665/4096 m/s² per LSB |
| ssp-gyro | `in_anglvel_*` | type 1; 1/16.4 °/s → 0.001064225 rad/s |
| ssp-magn | `in_magn_*` | type 4 (calibrated); 0.06 µT = 0.0006 G (**unverified**) |
| ssp-light | `in_illuminance_input`, `in_intensity_{red,green,blue,clear}_raw` | type 9; lux is an ams-style approximation (`light_dgf=310`) |
| ssp-prox | `in_proximity_raw` (8-bit count) | type 7; near if ≥ 130 |
| ssp-pressure | `in_pressure_raw/scale` (Pa→kPa), `in_temp_raw/scale` | type 5; temperature unit is a guess |

- A sensor is ADDed in the hub on its first raw read. The read waits up to
  `first_sample_ms` for data.
- The sensor is REMOVEd after `idle_ms` (3000) without reads.
- Reads return `-EAGAIN` until the MCU is ready.
- No buffers or triggers are provided: the kernel has no `IIO_KFIFO_BUF`, and
  iio-sensor-proxy's poll drivers only need raw reads.
- Udev hint for proximity: `PROXIMITY_NEAR_LEVEL=130`.
- The bbd-ssp device also has `enable` (downstream bitmask semantics), `enable_irq`,
  `mcu_rev`, `mcu_reset`, `sensor_state`, `ready` and `stats`.

## What changed / dropped / stubbed

- **Dropped:**
  - Downstream tty plumbing: `bbd_tty.c` (kernel `filp_open("/dev/ttyBCM0")` + `set_fs`)
    and `bbd_sio.c`.
  - The fake serial_core UART.
  - The bridge now talks to the SPI driver directly. The tx path is a kfifo drained by the
    SPI worker. The rx path is a kfifo plus an rx worker that calls `BbdBridge_SetData`.
  - `BBD:Serial=TTY/SPI` both just open the engine.
- **Dropped:** Samsung factory sysfs (`/sys/class/sensors/*`), input devices,
  `ssp_sensorhub` library channel and misc devices, MCU dump/big-data tasks, mobeam,
  temp/humidity, shake-cam → AF hook, batch ioctl, early-suspend, and `bootmode`.
- **Stubbed:**
  - Accel/gyro/prox/pressure calibration from `/efs` files. Offsets are 0.
  - `bcm4773_mcu_resp_float()` is a no-op (pinctrl handles the pull).
  - `ssp_check_sec_dump_mode()` returns 0.
- **SPI:**
  - 8-bit words by default. bbd.ko parameter `bpw32=1` restores downstream's 32-bit words
    for long half-duplex writes, which only works if the controller byte-swaps.
    Mainline spi-s3c64xx does not.
  - DMA-safe kmalloc buffers.
  - Transfers are serialised with a mutex.
  - The IRQ depth hacks were removed.
- **Fixed while porting:**
  - Overflow of the 4 KiB rx buffer in `bbd_write` (downstream allowed 8 KiB).
  - `bbd_pull_packet` could not wait.
  - Leaked timed-out SSP messages.
  - Use-after-free in `get_time`.
  - Unbounded frame parsing in `parse_dataframe` and `bbd_on_packet_work_func`.
  - `sscanf("%s")` overflow in sysfs.
  - The SSI debug *read* path is now functional and copies into `d.buf`. Downstream never
    returned data. The *write* (patch download) path is byte-identical, including the
    "hoi test" 16-byte padding.

## TODO / risks

- Build status: both modules build with clang (LLVM=1) against build/kernel.
  - The only warnings are downstream-style `-Wmissing-prototypes` and `-Wignored-qualifiers`.
  - Every undefined symbol was checked against System.map `__ksymtab_*`. The only ones
    not in vmlinux are 6 IIO symbols, which industrialio.ko exports.
  - CONFIG_MODVERSIONS is off, so the missing CRCs do not matter.
- Nothing has been tested on hardware.
- LHD (bionic) still has to run under libhybris or the Android linker. The download
  sequence remains closed (see the notes).
- Verify on the phone:
  - the magnetometer/pressure/temperature units;
  - the lux formula;
  - whether the hub output frame needs a non-identity mount matrix.
- Calibration storage (DT or module parameters) is still missing.
- Optional IIO buffers once `IIO_KFIFO_BUF` is enabled.
- `bbd-ssp` uses `device_set_of_node_from_dev()` to share the SPI node. A proper binding
  would put a child node under the SPI node.
- Suspend: the hub gets MSG2SSP_AP_STATUS_SUSPEND/RESUME. The HOST_REQ IRQ is a wake
  source (`enable_irq_wake`).
