# Mainline Linux on the Samsung Galaxy Note 4 (Exynos, SM-N910C "treltexx")

postmarketOS edge + Phosh on an **upstream 7.3 kernel** on the Exynos 5433 Galaxy Note 4.
The board description reuses the arm64 Exynos 5433 / Samsung TM2 device tree, but the kernel
runs in **AArch32** (see [Why 32-bit](#why-32-bit)).

> Status: daily-usable bring-up on one test device (SM-N910C, hw rev 21, S-Boot N910CXXS2DRI3).
> No telephony. Flashing a custom boot image is at your own risk.

## What works

| Area | State | Notes |
|---|---|---|
| CPU | ✅ 8 cores (4×A53 + 4×A57) | SMP via Samsung SMC + SYSRAM_NS boot address; `capacity-dmips-mhz` so UI runs on the A57s |
| cpufreq / thermal | ✅ | both clusters, step_wise throttling |
| Display | ✅ | S6E3HA2 1440×2560 DSI command-mode panel, DECON |
| GPU | ✅ panfrost (Mali-T760 MP6) | G3D_PLL fixed at 900 MHz, DVFS by divider only: 150–450 MHz |
| Touchscreen | ✅ | STMicro FTS (stmfts) with vendor init fixes |
| Touch keys + backlight | ✅ | Cypress tm2-touchkey; Back / Recents→Super; backlight follows panel |
| Buttons, flip-cover hall | ✅ | Home → Super (Phosh overview), hall as `SW_LID` |
| Vibration | ✅ | max77693-haptic |
| Battery / charger | ✅ | MAX77843 fuel gauge + minimal charger driver |
| USB gadget networking | ✅ | NCM, ~2 ms RTT (USB2 LPM disabled in gadget mode) |
| Wi-Fi | ✅ | BCM4358 over PCIe, `brcmfmac` with the stock firmware/NVRAM (re-probe service works around a missed first probe) |
| Sensors | ✅ | BCM4773 sensor hub: accel, gyro, magnetometer, light, proximity, pressure → IIO → iio-sensor-proxy (auto-rotate, proximity) |
| Audio (WM5110) | ❌ | modules blacklisted, not done yet |
| Bluetooth (BCM4358) | ❌ | not done yet |
| Modem, cameras, S-Pen, GNSS | ❌ | no mainline support / not attempted |
| Suspend | ❌ | lid/suspend disabled in logind |
| HW video decode (MFC) | ❌ | needs firmware from the stock ROM |

Known limitation: the Mali-T760 is fill-rate bound at 1440×2560, so GTK4 apps (e.g. Settings)
stutter while Phosh itself (GTK3) is smooth.

## Repository layout

```
kernel/patches/     16 patches on top of linux 7.3.5 (format-patch, in apply order)
kernel/config/      config fragments: note4.config (built-in), note4-keep.config (modules), lsmod
dts/                exynos5433-treltexx.dts + sensor-hub fragment (same as in the patches)
pmaports/           device-samsung-treltexx and linux-samsung-treltexx packages (incl. mkdtbh.py)
sensorhub/          out-of-tree bbd.ko + ssp_bbd.ko (BCM4773 BBD bridge + SSP → IIO), ported from
                    the Samsung/LineageOS 3.10 kernel; see sensorhub/README.md
rootfs/             files that currently live only on the device (systemd units, udev, logind, modprobe)
tools/              helper scripts (fast repack, ssh/telnet helpers, iio-sensor-proxy rebuild)
initramfs/          minimal debug initramfs init used during early bring-up
docs/               project log (Russian) and research notes
```

## Boot specifics

- **S-Boot only starts AArch32 kernels.** Load addresses: zImage `0x20008000`, DTB `0x21f00000`,
  initrd `0x22000000`; Image + zImage must fit in ~31 MB.
- The device tree has to be packed as a Samsung **DTBH** `dt.img` (platform `0x3ec3`, subtype
  `0x3a37307e`, chip 5433); `pmaports/linux-samsung-treltexx/mkdtbh.py` does that, and
  `mkdtbhbootimg` (LineageOS `hardware/samsung`) builds a boot.img with the `--dt` section.
  Boot image: base `0x10000000`, pagesize 2048, ramdisk offset `0x01000000`, tags offset `0x100`.
- S-Boot overwrites the command line, so the kernel uses `CONFIG_CMDLINE_FORCE`.
- The BOOT partition is 14 MiB → `deviceinfo_create_initfs_extra="true"`.
- The firmware leaves CNTVOFF unset on secondary cores → the DT marks the arch timer
  `arm,cpu-registers-not-fw-configured`.
- **G3D_PLL must not be re-locked at runtime** (the GPU hangs): it is set once at probe and
  devfreq only changes the divider.

## Building

### pmOS packages
Copy `pmaports/*` into your pmaports checkout under `device/testing/` and run
`pmbootstrap init` (vendor `samsung`, device `treltexx`), `pmbootstrap install`, then flash
with Heimdall in Download mode (Vol Down + Home + Power, then Vol Up):

```
heimdall flash --BOOT boot.img          # pmbootstrap export gives you boot.img
```
The rootfs was installed to USERDATA.

### Fast kernel loop (what this port was developed with)
```
cd linux-7.3.5 && git am ../kernel/patches/*.patch
# config: pmOS linux-postmarketos-exynos5 config + kernel/config/note4.config (+ note4-keep.config)
make ARCH=arm LLVM=1 LOCALVERSION= -j$(nproc) zImage modules samsung/exynos5433-treltexx.dtb
tools/repack.sh                          # zImage + DTBH dt.img + pmOS ramdisk → out/pmos-boot-local.img
heimdall flash --BOOT out/pmos-boot-local.img
```
`LOCALVERSION=` and `CONFIG_LOCALVERSION_AUTO=n` keep the kernel release equal to the installed
modules.

### Sensor hub
The hub needs Broadcom's **closed `lhd` daemon** (from the stock/LineageOS 18.1 system image) and the
hub **firmware patch** (shipped as `drivers/sensorhub/brcm/bbdpl/bbd_patch_file.h` in the downstream
universal5433 kernel). Neither is in this repo:

1. Build `sensorhub/` against the kernel tree (`make -C <kernel> M=$PWD ARCH=arm LLVM=1`), install
   `bbd.ko` and `ssp_bbd.ko` into `/lib/modules/<release>/extra`, and run `depmod`.
2. Convert it with `sensorhub/tools/extract_bbd_patch.py bbd_patch_file.h bcm_sensorhub_a2.patch` → `/lib/firmware/bcm4773/bcm_sensorhub_a2.patch`.
3. Build a bionic chroot for `lhd` at `/var/lib/lhd/root` with stub `libgui`/`libicuuc`/`libhardware_legacy`
   (`sensorhub/tools/*.c`), as described in `docs/notes/lhd-runtime.md`.
4. Install `rootfs/` (lhd.service, modules-load, udev rules) and run `systemctl enable --now lhd`.

The firmware rotates the axes itself; `61-note4-accel.rules` adapts them to the iio-sensor-proxy
convention and sets the proximity threshold. `tools/iio-sensor-proxy-build.sh` optionally rebuilds
iio-sensor-proxy with a 60° rotation threshold instead of 35°.

## Why 32-bit

The Exynos 5433 is ARMv8, but Samsung's signed S-Boot and secure firmware on retail Note 4 units
hand over to the kernel in AArch32, and the EL3 monitor offers no call to change execution state
(no PSCI, legacy Exynos SMC interface only). The Samsung TM2 reference phones run mainline arm64 only
because their bootloader is 64-bit. As far as public sources go, nobody has booted arm64 on a retail
5433 phone; see `docs/notes/arm64-prior-art.md`.

## Credits / licenses

- Kernel patches, device tree and `sensorhub/` drivers: GPL-2.0-only (the sensor-hub code is derived
  from Samsung's GPL kernel release for this device).
- Device tree based on the upstream Exynos 5433 / Samsung TM2 work.
- postmarketOS, panfrost/Mesa and the LineageOS `trelte` maintainers, whose trees were invaluable as references.

No proprietary blobs, firmware, or device backups are included.
