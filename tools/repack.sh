#!/bin/sh
# Fast loop: rebuild boot image from build/kernel zImage + DTB and pmOS ramdisk.
set -e
cd "$(dirname "$0")/.."
W=build/repack
python3 pmaports/device/testing/linux-samsung-treltexx/mkdtbh.py -o $W/dt-new.img --chip 5433 --platform 0x3ec3 \
	--subtype 0x3a37307e --rev 0:255 build/kernel/arch/arm/boot/dts/samsung/exynos5433-treltexx.dtb
tools/mkdtbhbootimg --kernel build/kernel/arch/arm/boot/zImage --ramdisk $W/pmos-boot.img-ramdisk.gz --dt $W/dt-new.img \
	--cmdline "$(cat $W/pmos-boot.img-cmdline)" --base 0x10000000 --pagesize 2048 --ramdisk_offset 0x01000000 \
	--tags_offset 0x00000100 -o out/pmos-boot-local.img
ls -la out/pmos-boot-local.img
