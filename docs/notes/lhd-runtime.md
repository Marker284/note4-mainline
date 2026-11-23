# Running stock `lhd` (bionic, armv7) on postmarketOS: runtime notes

Date: 2026-11-23. Source of facts: the stock LineageOS 18.1 system partition (/dev/mmcblk0p18, mounted ro
at /mnt/sys on the phone) and a copy pulled to the host. The host-side analysis was done with readelf on
the copy. On-device runtime tests were **not** run because SSH to the phone broke mid-task (see the list at the end).

## 1. What lhd actually needs

The binary is /system/vendor/bin/lhd (no separate vendor partition, so /vendor points to /system/vendor).
- It is a 32-bit ARM PIE. `PT_INTERP` = `/system/bin/linker`, built for API 23, `BIND_NOW`.
- On the image, /system/bin/linker is a symlink to `/apex/com.android.runtime/bin/linker`.
  The APEXes are *flattened*: they sit in `/system/apex/<name>/` as plain directories, and /apex on the image is an empty mountpoint.
- A real copy of the linker is also at /system/bin/bootstrap/linker.
- The config it really uses is `/system/etc/lhd.conf`, per init.universal5433.rc:
  `service lhd /vendor/bin/lhd /system/etc/lhd.conf`, user gps, groups system inet net_raw.
  /system/vendor/etc/lhd.conf does not exist on this image.

DT_NEEDED: liblog, libicuuc, libutils, libgui, libhardware_legacy, libc++, libdl, libc, libm.

Imported symbols (the full UND list):
- libc: open/read/write/ioctl, termios, select, sockets (socket/bind/listen/accept/connect), pthreads,
  fork/execv/popen/waitpid, mkfifo, stdio, and the `_chk` fortify functions.
- liblog: `__android_log_buf_write`, `__android_log_vprint`.
- libhardware_legacy: `acquire_wake_lock`, `release_wake_lock`. lhd ignores their return values.
- libutils: `android::elapsedRealtime()`.
- libc++: operator new/delete, `__cxa_guard_*`, `__cxa_pure_virtual`.
- **Nothing at all from libgui or libicuuc.** Those are dead NEEDED entries.
- **No `__system_property_*` imports.** lhd does not read properties itself. Only libc, liblog and libbase
  read them internally, and they fall back to defaults when the property area is absent.

### Full transitive closure

The unmodified closure is 63 libs, about 8.7 MB:
- 58 from /system/lib
- libc/libm/libdl/libdl_android from /apex/com.android.runtime/lib/bionic
- libicuuc from /system/apex/com.android.art.release/lib

libgui alone drags in most of the closure: binder, hidl, EGL/GLES, gralloc mapper HALs, libui, libinput, libselinux and so on.

libhardware_legacy (Android 11) also pulls in libbinder, libhidlbase and android.system.suspend@1.0.
Its `acquire_wake_lock` calls `ISystemSuspend::getService()` over **hwbinder**:
- **Without /dev/hwbinder**, libhidl returns nullptr, so the call fails fast and is harmless.
- **If /dev/hwbinder exists** (binder enabled in the kernel), it waits forever for the `hwservicemanager.ready` property, so lhd hangs on its first log call (lhd takes a wakelock in its logger).

Reduced closure with three stub libs (libgui, libicuuc, libhardware_legacy), 12 libs:

    libc.so libm.so libdl.so libdl_android.so   (apex runtime bionic)
    ld-android.so liblog.so libc++.so libutils.so libcutils.so libbase.so
    libprocessgroup.so libcgrouprc.so libvndksupport.so  (system/lib)

### Paths and files lhd touches (from strings and the decompile)

- /dev/bbd_{patch,packet,reliable,control,sensor} and /sys/class/sec/gps/GPS_PWR_EN/value (the latter is
  set by lhd.conf GpioNStdbyPath; it must be writable).
- /sys/devices/platform/**bbd.0**/{DEV,debug,pl}: the 3.10-era platform device name. A mainline port registers
  "bbd" (no ".0") unless the id is forced. /proc/ssp.
- /data/system/gps/ (NvStorageDir, `CtrlPipe` FIFO via mkfifo, mode 0660), /data/gps/.lhd, /data/etc/lhd.conf (optional
  override), /.pipe.gpsd_to_lhd, /var/log/gl. The log dir /storage/emulated/0/... is unused while LogEnabled=false.
- `PortName=tcp:127.0.0.1:300` makes lhd **listen on TCP 127.0.0.1:300**, a privileged port that needs root or
  CAP_NET_BIND_SERVICE. Both the decompile and the strings confirm bind/listen.
- Logging:
  - liblog writes to /dev/socket/logdw. In a chroot that socket is absent, so those messages are silently dropped.
  - lhd.conf `Lhe477xDebugFlags=...:STDOUT_PUTS:STDOUT_LOG` sends lhd's own log to stdout, which ends up in journald.
- SELinux: lhd needs none. libselinux appears only in the libgui closure, and its constructor only probes /sys/fs/selinux.

## 2. Options

| Option | Verdict |
|---|---|
| (a) bionic linker in a chroot/RootDirectory | **Recommended.** Bionic does not need Android init. With no property area, `__system_properties_init()` fails and libc ignores the failure, so every property read returns "", which means defaults. With no /linkerconfig/ld.config.txt and no /system/etc/ld.config.txt, the linker logs one warning and falls back to a no-config default namespace with search path /system/lib:/odm/lib:/vendor/lib. LD_LIBRARY_PATH is honored. No ANDROID_ROOT or ANDROID_DATA env is needed for this binary. |
| (b) libhybris | **Not viable.** Nothing matching `*hybris*` is in Alpine edge main/community/testing armv7 or in the pmOS master + systemd armv7 APKINDEX (checked 2026-11-23 from the host). libhybris targets glibc processes that dlopen bionic .so files. It adds nothing for a standalone bionic executable and has no musl support. |
| (c) LXC / Halium container | Overkill. `lxc` is in Alpine community, but a Halium-style container needs Android init, property service, servicemanager/hwservicemanager and binder in the kernel. That is a lot of surface for one daemon that uses 5 non-libc symbols. Keep it in mind only if gpsd/GNSS (libgui, binder HALs) is ever wanted too. |

## 3. Recommended recipe (option a, using a copy rather than the live partition)

Build a self-contained root at /var/lib/lhd/root.
- It does not depend on mmcblk0p18 staying mounted, and nothing gets mounted over the read-only partition.
- No /apex bind mounts are needed, because the apex libs go directly into /system/lib.

```sh
M=/mnt/sys/system  R=/var/lib/lhd/root
mkdir -p $R/system/bin $R/system/lib $R/system/vendor/bin $R/system/etc $R/data/system/gps $R/data/gps \
         $R/dev $R/proc $R/sys $R/tmp $R/var/log
cp $M/apex/com.android.runtime/bin/linker            $R/system/bin/linker      # real file, not the symlink
cp $M/apex/com.android.runtime/lib/bionic/lib{c,m,dl,dl_android}.so $R/system/lib/
cp $M/lib/{ld-android,liblog,libc++,libutils,libcutils,libbase,libprocessgroup,libcgrouprc,libvndksupport}.so $R/system/lib/
cp $M/vendor/bin/lhd $R/system/vendor/bin/; cp $M/etc/lhd.conf $R/system/etc/
cp ./ref/lhd-stubs/lib{gui,icuuc,hardware_legacy}.so $R/system/lib/    # see below; or copy the full 63-lib closure instead
ln -s /system/vendor $R/vendor
```

Building the stubs on the host:
- Use clang/lld, linking against the phone's own bionic libc.so.
- Tested to build, with the result checked by readelf.
- Build it from a scratch directory that holds the two .c files below. `LIBC` must point at the phone's own bionic libc.so, i.e. a copy of `/system/apex/com.android.runtime/lib/bionic/libc.so`.

```sh
T="--target=armv7a-linux-androideabi23 -fPIC -shared -nostdlib -fuse-ld=lld -O2"
clang $T -Wl,-soname,libgui.so    -o libgui.so    empty.c
clang $T -Wl,-soname,libicuuc.so  -o libicuuc.so  empty.c
clang $T -Wl,-soname,libhardware_legacy.so -o libhardware_legacy.so hwlegacy.c $LIBC
```
`hwlegacy.c` implements acquire_wake_lock and release_wake_lock as writes of the id to /sys/power/wake_lock and
/sys/power/wake_unlock. That needs CONFIG_PM_WAKELOCKS, and the stub returns -1 otherwise.

systemd unit /etc/systemd/system/lhd.service:
```ini
[Unit]
Description=Broadcom LHD (BCM4773 sensor hub / GNSS loader)
After=systemd-udev-settle.service
ConditionPathExists=/dev/bbd_control
[Service]
RootDirectory=/var/lib/lhd/root
MountAPIVFS=yes            # /proc /sys /dev inside the root
ExecStart=/system/vendor/bin/lhd /system/etc/lhd.conf
WorkingDirectory=/
Environment=LD_LIBRARY_PATH=/system/lib
User=root                  # simplest: port 300 + GPIO sysfs; else a gps user + AmbientCapabilities=CAP_NET_BIND_SERVICE
Restart=on-failure
[Install]
WantedBy=multi-user.target
```
Manual test without systemd: `chroot $R /system/bin/linker --list /system/vendor/bin/lhd` (ldd mode), then
`chroot $R /system/vendor/bin/lhd /system/etc/lhd.conf` with /proc, /sys and /dev bind-mounted.

## 4. Pitfalls

- **Do not expose /dev/hwbinder** to lhd while using the real libhardware_legacy, or lhd hangs (see §1). The stub removes the issue.
- **/sys paths are 3.10-era.** lhd expects /sys/devices/platform/bbd.0/... and /sys/class/sec/gps/GPS_PWR_EN/value (a gpio_export
  link). The kernel port has to recreate those names, for example with a compat sysfs class and a symlink, or lhd fails before ESW:READY.
- **Port 300** must be free and bindable. To avoid needing root, edit PortName in lhd.conf to a high port (gpsd is not used anyway).
- liblog output is lost without logd. If needed, add an LD_PRELOAD shim (bionic honors LD_PRELOAD) that redefines
  `__android_log_buf_write` and `__android_log_vprint` to write to stderr.
- The bionic libs keep libc's fortify, fdsan and `PR_SET_VMA` calls. On mainline these only return EINVAL, which is harmless.
- **Never put /system/lib in the musl dynamic loader's search path.** The chroot keeps musl and bionic apart.
- **Keep the binary and the patch together.** The firmware patch comes through /dev/bbd_patch, which the kernel driver serves from bbd_patch_file.h, so the kernel blob and lhd must come from the same build.

## 5. On-device checks still to run (SSH was down)

1. `zcat /proc/config.gz | grep -E 'ANDROID_BINDER|PM_WAKELOCKS|SECURITY_SELINUX='`; `ls -l /dev/*binder* /dev/bbd* /sys/class/sec`
2. Build the root as in §3, then `chroot $R /system/bin/linker --list /system/vendor/bin/lhd`.
   This confirms that the linker works without a property area or ld.config. Expect a single linkerconfig warning.
3. `chroot $R /system/vendor/bin/lhd /system/etc/lhd.conf` only after the bbd driver is loaded, to see where it stops.
   Watch for opens of /sys/devices/platform/bbd.0 and GPS_PWR_EN (`strace -f` from the host side works on bionic processes).
4. `ss -ltn | grep :300` to confirm the TCP listener.
