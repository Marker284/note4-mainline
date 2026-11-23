# arm64 on Note 4 (Exynos 5433): prior art and options

Research notes, 2026-11-23. Only web reading and existing files in ref/. Nothing was run on the device.

## 1. Prior art: 64-bit kernel from a 32-bit Samsung bootloader

- **I found no public report of an arm64 kernel booting on a retail Exynos 5433 phone** (Note 4 N910C/H/U, Tab S2 T710/T810/T715/T815).
- The postmarketOS wiki page for treltexx says (quoted in search results; the page itself is behind Anubis): to boot an arm64 mainline kernel on exynos5433 devices "we would need to get our hands on these firmware files, or in some other way create 64bit sboot/trustzone firmware for exynos5433". The same text appears on the tre3calte page.
  https://wiki.postmarketos.org/wiki/Samsung_Galaxy_Note_4_(samsung-treltexx)
- pmaports: samsung-treltexx and gts210lte are armv7 downstream-kernel packages. None is aarch64.
  https://pkgs.postmarketos.org/package/main/postmarketos/armv7/linux-samsung-treltexx
- Galaxy Alpha and A8 (2015) use the **Exynos 5430**, and the Meizu MX4 Pro also uses a 5430. That chip has Cortex-A15/A7 cores (ARMv7 only), so arm64 is impossible there.
- Press coverage from 2014 confirms the 5433 is ARMv8 but shipped in AArch32 mode on the Note 4:
  https://www.anandtech.com/show/8718
- The Galaxy S6 (Exynos 7420) is a counter-example: its SBOOT itself is AArch64 and based on ARM Trusted Firmware BL31, so its kernel is natively AArch64. Quarkslab:
  https://blog.quarkslab.com/reverse-engineering-samsung-s6-sboot-part-i.html
  https://blog.quarkslab.com/reverse-engineering-samsung-s6-sboot-part-ii.html
- Our downstream 3.10 tree (ref/ds-tree.txt) only contains the generic arch/arm64 skeleton, with no Exynos5433 arm64 machine code.

## 2. How TM2/TM2E boots in AArch64

- TM2 and TM2E were Samsung-internal Tizen reference phones. Their firmware chain (Samsung S-Boot plus secure firmware) is **64-bit** and enters the kernel in AArch64. Mainline DTS was added in v4.10 (Chanwoo Choi, 2016):
  https://lore.kernel.org/linux-arm-kernel/1472046551-703-7-git-send-email-cw00.choi@samsung.com/
  https://lists.openwall.net/linux-kernel/2016/11/18/336
- U-Boot for TM2 (Marek Szyprowski, Nov 2018, series "ARM: Exynos: Add TM2 board support") is **loaded by the vendor S-Boot in place of the kernel**. It decompresses and starts Linux or provides THOR download. It does not replace the secure firmware.
  https://lists.denx.de/pipermail/u-boot/2018-November/346782.html
  https://lists.denx.de/pipermail/u-boot/2018-November/348417.html
  Mainline U-Boot today has no TM2 target. It does have espresso7420.
- Secondary CPUs on TM2 use **PSCI over SMC**. From mainline `arch/arm64/boot/dts/exynos/exynos5433.dtsi`:
  ```
  psci { compatible = "arm,psci"; method = "smc";
         cpu_off = <0x84000002>; cpu_on = <0xc4000003>; };
  ```
  All CPUs use `enable-method = "psci"`. The boot CPU is in the A53 cluster (MPIDR 0x100). Per the 2014 LKML thread, the firmware is PSCI v0.1, CPU_OFF/hotplug was not working, and EL2 entry was not considered:
  https://lists.openwall.net/linux-kernel/2014/12/02/303
- **Note 4 firmware uses a different interface.** There is no PSCI (`CONFIG_ARM_PSCI` is not set in lineage_trelte_defconfig). The downstream kernel uses the legacy Exynos SMC table (ref/ds-smc.h: SMC_CMD_CPU1BOOT = -4 and so on) plus a boot address at SYSRAM_NS+0x1c. Our patch 0002 writes that address and powers the core through the PMU. So the Note 4 firmware is the "Android 32-bit" build of Samsung's secure firmware, and it does not offer the TM2 PSCI interface. I found no public analysis that confirms or rules out a shared code base.
- The modern Samsung SMC header (exynos-smc.h, e.g. universal2100) lists no "switch NS execution state" command.

## 3. General ARMv8 techniques: AArch32 EL1/EL2 to AArch64 kernel

The architectural fact behind all of these: lower ELs can only become AArch64 through an exception return from a higher EL whose RW control (SCR_EL3.RW, HCR_EL2.RW) selects AArch64. AArch32 EL1/EL2 cannot change its own state, and RMR exists only at the highest implemented EL (EL3 here). **Every route therefore needs cooperation from the EL3 monitor.**

| Technique | Who provides it | Applies to Note 4? |
|---|---|---|
| Qualcomm SCM "milestone 32→64" (SIP fast call 0x10f, `el1_system_param` with x0..x8 and ELR) | Qualcomm TZ; LK `scm_elexec_call()` | Qualcomm only. Shows that vendors do ship such a call. https://github.com/msm8916-mainline/lk2nd/blob/main/platform/msm_shared/scm.c , U-Boot snapdragon boot0.h workaround: https://qemu.googlesource.com/u-boot/+/00cac7456125a2ab76ea30d32d78f4a2b177b2b3/arch/arm/mach-snapdragon/include/mach/boot0.h |
| TF-A Arm SiP `ARM_SIP_SVC_EXE_STATE_SWITCH` | Upstream TF-A, Arm platforms | Only if the monitor is TF-A with this service. The Samsung SMC lists show none. https://trustedfirmware-a.readthedocs.io/en/v2.12.0/components/arm-sip-service.html |
| PSCI CPU_ON with an AArch64 entry | PSCI firmware | Note 4 firmware has no PSCI as far as is known. Per the PSCI spec, the target core's entry state also follows the caller's state (SMC32 from AArch32), so this would not switch states. |
| RMR warm reset (Allwinner A64 SPL, RPi3 armstub) | Code already at EL3 | Not available to us: we never run at EL3. https://lists.denx.de/pipermail/u-boot/2016-December/276373.html |
| Boot firmware rebuilt/replaced with a 64-bit chain (TM2-style) | Signed vendor images | Excluded: not allowed under our constraints and not possible on retail units (see section 4). |

**Most promising non-flashing route:** a careful, documentation-level check of whether the Note 4's existing secure firmware exposes any *legitimate, documented* AArch64 entry option. Examples: a PSCI function ID answered from AArch32 (probe PSCI_VERSION 0x84000000 and treat NOT_SUPPORTED as the answer), or a state flag consulted on the warm-boot path. If the answer is no, which is what all public evidence suggests, a 32-bit kernel (the current approach) is the end state. I did not and will not look for ways to subvert or exploit the monitor.

## 4. Secure-boot constraints (high level)

- On retail Exynos phones, the BootROM verifies BL1 and the chain of trust continues through BL2, S-Boot and the secure firmware. These stages are Samsung-signed and cannot be replaced. Custom boot.img (kernel) is accepted, at the cost of tripping the Knox warranty bit.
- So the only levers are what we put in boot.img and which interfaces the running firmware exposes. Replacing or modifying BL1/BL2/S-Boot/TZSW is out of scope.

## Risks

- Probing unknown SMC IDs on a monitor that shares state with the TEE can hang the device or put it into an unexpected reset. It cannot brick it, because nothing is written to flash.
- Even if AArch64 entry were achieved for CPU0, secondaries, CPU hotplug and idle/suspend paths in the monitor would likely still return to AArch32. This is the same class of problem as the Qualcomm "PSCI bug".
- CNTVOFF/timer quirks (see patch 0010) and the missing EL2 would remain.
