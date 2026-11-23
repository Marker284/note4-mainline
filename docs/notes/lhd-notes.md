# Stock `lhd` (Broadcom Location Hub Daemon): how it boots the BCM4773 sensor hub

Date: 2026-11-23. Static analysis only: SSH to the phone broke mid-task, and Ghidra was stopped on request.
Companion notes: sensorhub-notes.md (the kernel side) and lhd-runtime.md (running the binary on pmOS).

## Files (ref/lhd/)

From the stock LineageOS 18.1 /system (mmcblk0p18, mounted ro):
- **lhd**: /system/vendor/bin/lhd, 232008 B, BuildID 6db09ce1…, sha256 fbe518f0….
  - Version string: "Broadcom LHD ver …208030+patch[n] RPC=207067, built Jul 14 2022".
  - Built from a Samsung "T/WNRO2022" tree.
- **lhd.conf**: /system/etc/lhd.conf. There is no /system/vendor/etc copy.
- **gps.xml**: /system/etc/gps.xml, used by gpsd.
- init.universal5433.rc and init.samsung.rc from /system/vendor/etc/init/hw.
- sensorhubservice and libsensorhub.so (Samsung extras, not needed).
- Derived files:
  - lhd.strings (`strings -t x`)
  - lhd.thumb.annot.s: full Thumb disassembly with every PC-relative string reference resolved (`; =0xADDR "string"`)
  - lhd.decomp.c and lhd.decomp.resolved.c: Ghidra decompile, first pass only. **Ghidra addresses = file vaddr + 0x10000.**
  - lhd.decomp.funcindex.txt

**There is no .patch file on /system.** lhd.conf has `LhePatch=/dev/bbd_patch`; the /system/bin/*.patch lines are commented out.
- The firmware therefore comes from the kernel. The char array in bbdpl/bbd_patch_file.h is served by /dev/bbd_patch.
- lhd labels that source "text(kernel)" (string 0x32e0c).
- Copy: ref/sensorhub-ds/bbdpl/bbd_patch_file.h.

init snippets (init.universal5433.rc):

    on boot (l.371-379):
        chown root system /sys/class/sec/gps/GPS_PWR_EN/value ; chmod 0664 …
        mkdir /data/system/gps 0771 system system ; rm /data/system/gps/{gps_started,glonass_started,beidou_started,smd_started,sv_cno.info}
    service lhd /vendor/bin/lhd /system/etc/lhd.conf   (l.630; class main, user gps, group system inet net_raw, ioprio be 0)
    service gpsd /vendor/bin/gpsd -c /system/etc/gps.xml (l.652; GPS only, talks to lhd)

Config defaults (LhdConfig ctor at file 0x9418, parser at 0x9618):
- GpioDelayMs = 20
- LogEnabled off
- Any `Lhe*` key is passed to the Lhe parser (0xee68).
  - `LheBbd*` sets the "BBD mode" flag (Lhe+0xfb1).
  - `LheBbdControl`, `LheBbdPacket`, `LheBbdReliable`, `LheBbdSensor`, `LheBbdSsi` and `LheBbdSio` set the node paths (0xc97c-0xcab4).
  - `LheBbdSsi` is **not** set, so the SSI-SPI debug write path is unused.

## Encodings recovered

- **Control strings.** `open(path, O_RDWR)`, then `write(fd, s, strlen(s)+1)` (the NUL is included), at file 0xe320/0xe434.
- **Kernel side.** bbd_control_write → bbd_ESW_control → strstr matching (bbd_control.c:166-258).
- **Stream codec** (vtable 0x386e8, 0x1fd54-0x1fe28). PutU8/PutU16/PutU32 are plain **little-endian** and PutBuf is raw bytes.
- **RPC framing** (0x2d084, ghidra; identical to the kernel's BbdBridge_AddRpc):
  - id: 1 byte, or 2 bytes `(id>>8)|0x80, id` when id ≥ 0x80
  - len: the same 1- or 2-byte scheme
  - then the payload
  - Several RPCs are packed into one transport-layer (TL) packet of up to 0x400 bytes, then sent by TL (0x2d5a8).
- **TL framing.** This is the same Broadcom transport layer as the kernel's transport/transport_layer_c.c, so the kernel source documents the framing.

Patch container (patch_reader.cpp):
- Text tags: `<FormatVersion> <Crc> <BlobLength> <AsicVersion> <ChangeList> <PatchLevel> <PostPatchLevel> <Name> <Type> <PatchAddress> <Blob=hex>`.
- Validation (0x30800 ghidra):
  - AsicVersion must equal the version the chip reports. 0x47730A20 is allowed for a chip that reports 0x4773a0.
  - ChangeList must be ~0 or equal the ROM version.
  - PatchLevel must equal the chip's patch version.
  - BlobLength must not be 0.

The blob is a stream of **patch-engine records**: `[type u8][len u16 LE][payload]` (pe.cpp, 0x35fd8/0x35de4).

| type | payload | handler (vtable 0x38800) | RPC sent |
|---|---|---|---|
| F1 | addr32 + data | 0x20c10 | **id 5** WriteMem(u32 addr, u16 len, bytes) |
| F0 | 3×u32 | 0x20c60 | id 6 (u32 addr, u32 val, u32 mask); masked write |
| F5 | 3×u32 | 0x20c8c | id 7 (u8 0, u32 addr, u16 4); read and verify |
| F2 | u32,u32,u8 | 0x20cc4 | id 0x31 (addr, len) + expected CRC8 |
| F3 | 3×u32 | 0x20cf4 | id 0x30 (addr, len) + expected checksum32 |
| F4 | addr + ≤4 u32 | 0x202fc | not sent at once: stored as a service-list entry |

The real blob (bcm_sensorhub_a2) contains **only F1 records**:
- 325 records, mostly 912 data bytes each, covering 0x824000-0x86bef4. That is 294660 bytes, a Cortex-M vector table at 0x824000 (SP=0x0089b3f8, reset=0x00824401).
- The **last** record writes 16 bytes at 0x8243f0: `d91e7540 f47e0400 00408200 efbeadde`. That looks like crc, len=0x47ef4, base=0x824000, magic DEADBEEF.

Other ROM RPCs used by the downloader (proxy stubs 0x2bf40-0x2c2f4 ghidra):
- id 0: no args, RequestVersion
- id 0x37: (u32 fn, p0, p1, p2, p3), a call into a ROM function
- id 0x0b and id 0x2e: (u32 addr), start patch

## Reconstructed boot sequence

1. **Open the BBD nodes** (0xc700). Open /dev/bbd_control.
   - If the debug flag is set, write `BBD:DEBUG=1`.
   - Open /dev/bbd_packet and, for the download, write **`BBD:PassThru=1`** to control (0xc76e).
   - Then open /dev/bbd_reliable (plus sio/ssi if configured). /dev/bbd_sensor is opened separately.
2. **Power on** (AsicOn 0x100b4 → GPIO "Normal" 0xad00). Values are written as "0\n"/"1\n" to GpioNStdbyPath.
   - If the pin already reads 1: write 0, then usleep(20000). The log text says "120us", but the code sleeps 20 ms (0x4e20).
   - Write 1.
   - Sleep GpioDelayMs×1000 µs (20 ms).
   - "Standby" (0xad8c) writes 0.
3. **sysfs (best effort)** at 0x10118/0x101a8:
   - write '0'/'1' to /sys/devices/platform/**bbd.0**/debug
   - write the `LheSerialControl` value (`BBD:Serial=TTY`) to …/bbd.0/DEV
   - Our kernel names the device "bbd", so these fopen calls fail and are only logged. The TTY is already opened by bbd_main.
4. **Patch download** (SafeRecoverAsic 0x1078c → PatchDownload 0x105d8 → loader 0x2007c, firmware_downloader).
   - HAL Open("patch loader"), then control **`SSI:PatchBegin`**. SSP only logs this string.
   - Read /dev/bbd_patch as text and parse it.
   - The FirmwareDownloader runs **its own TL+RPC engine over the raw pass-through stream**, i.e. writes to /dev/bbd_packet go straight to ttyBCM0 → SSI-SPI. Its state machine (0x31090 ghidra):
     1. "Sync with 477x" (TL sync).
     2. "RequestingVersion": RPC 0. The reply gives AsicVersion, Rom and Patch, which are checked against the container.
     3. Clock setup for LheClockMHz=100 (0x30fb4): `SysClock_100=0x0005f153,3`, i.e. RPC 0x37(0x5f153, 3, 0, 0, 0).
     4. For non-A2 chips at 52 MHz only: masked writes to 0x400b0044. **Skipped for 0x47730A20.**
     5. "Downloading Patch": feed the blob in 0x200-byte slices to the PE parser, with at most 4 transactions in flight (0x30b1c). Each F1 record becomes an RPC 5 WriteMem.
     6. "Start Patch": RPC 0x0b(PatchAddress=0x00824401) followed by RPC 0x2e(0x00824401), then `usleep(bytes/11 + 20000)` µs, then "done".
   - When the downloader reaches 100%, it writes **`SSI:PatchEnd`** to control (0x2ffdc).
   - It logs "LHD Patch Load … KB took …".
5. **Hand over to the kernel TL.** HAL Open("LheIfc"). On Resync (LheEngine 0x2a1d0 ghidra):
   - ReqAsic(true)
   - **`BBD:PassThru=0`** to control (0x1a2b2)
   - the TL then sends **`PL:StartRemoteSync`** to control (0x1d87c / 0x2d86c ghidra), so the kernel's packet layer does the TL sync with the ESW
   - lhd waits for `PL:RemoteSyncComplete` on control reads (0x1d1a6). `PL:OnReliableError`, `PL:OnCommunicationError` and `PL:OnException:` mean failure.
6. **ESW:READY.** When the running ESW reports the hub up through an RPC callback (Lhe interface vtable 0x37cc0, slot 3 → 0x10ba8 → 0x10b7c), lhd writes **`ESW:READY`** to control. A callback with 0 writes `ESW:NOTREADY`. A crash writes `ESW:CRASHED` (0x1b638).
   - Kernel side: esw_ready=true, then on_mcu_ready, then msleep(1000), then initialize_mcu (ssp_bbd.c).
   - **The RPC id that triggers this callback was not resolved.** The incoming RPC dispatch is table-driven and Ghidra did not decompile it.
7. **Retries and reset.**
   - The MCU boot is retried, with "Tried %d times to boot MCU. Giving up." (MaxMcuResetTries, RsmResetTimeoutMS).
   - The kernel sends `BBD:RESET_REQ` (SHMD) on control, and lhd reruns SafeRecoverAsic (0x10328/0x103e4, "SHMD-->ESW reset_mcu").

## Android and runtime dependencies

- No property_get. Android imports:
  - `__android_log_*`
  - `android::elapsedRealtime`
  - `acquire_wake_lock`/`release_wake_lock`
  - libgui and libicuuc are dead NEEDED entries
- See lhd-runtime.md for the linker closure and stubs.
- Paths:
  - /dev/bbd_{control,packet,reliable,sensor,patch}
  - GPS_PWR_EN/value (must be writable)
  - /sys/devices/platform/bbd.0/{debug,DEV,pl}
  - /proc/ssp
  - /data/system/gps/ (NvStorageDir, mkfifo CtrlPipe)
  - /data/gps/.lhd and the .lhd.lock single-instance lock
  - /data/etc/lhd.conf (optional override)
  - /.pipe.gpsd_to_lhd (gpsd IPC)
  - /var/log/gl
  - /data/data/com.broadcom.test.yellowbird (log-forcing probe)
- Sockets: `PortName=tcp:127.0.0.1:300` makes lhd listen on a privileged port.
- No /dev/socket use was seen.
- It runs as user gps (needs the system group).

## Reimplementation sketch (no lhd)

The kernel already ships the patch, the TL and the PL-sync code. A minimal userspace or kernel loader:
1. Toggle the GPIO as in step 2.
2. Write `BBD:PassThru=1`.
3. Speak TL framing (transport_layer_c.c) over /dev/bbd_packet and send:
   - RPC 0 (check the version)
   - RPC 0x37(0x5f153, 3, 0, 0, 0)
   - 325 × RPC 5 WriteMem (the last one is the 16-byte header at 0x8243f0)
   - RPC 0x0b(0x824401) and RPC 0x2e(0x824401)
4. Sleep about 47 ms.
5. Write `BBD:PassThru=0`, then `PL:StartRemoteSync`, then wait for `PL:RemoteSyncComplete`, then write `ESW:READY`.

Open points:
- the ROM's version-reply layout
- whether ESW:READY must wait for a specific ESW RPC; writing it after the sync is the obvious first experiment
- the exact meaning of RPC 0x0b vs 0x2e

A trace from step 5 of option A (stock lhd with BBD debug on) would settle all three.
