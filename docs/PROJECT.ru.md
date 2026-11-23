# Samsung Galaxy Note 4 Exynos (SM-N910C/H, `treltexx`) → mainline Linux

Мастер-документ. Ведётся с 2026-11-22 (переписан после первого захода DeepSeek).

## 1. Суть подхода

**S-Boot и TrustZone Android-прошивки Note 4 запускают ядро только в AArch32.**
Стоковое ядро 32-битное (`arch/arm`, `vmalloc=` в bootargs, CPU описаны как
cortex-a7/a15, без PSCI). arm64-ядро с этого загрузчика не стартует, поэтому
старые mainline-попытки и висли на `Starting kernel`.

Поэтому собираем **32-битное mainline-ядро (`ARCH=arm`, multi_v7) для Exynos 5433**,
а описание SoC берём из arm64-DTS (`exynos5433.dtsi` + `exynos5433-tm2-common.dtsi`):
TM2 — это тот же референс-дизайн Samsung, что и Note 4.

Для этого нужен маленький патч Kconfig (`patches/0001-*.patch`): драйверы клоков
и пинов 5433 разрешены только при `ARM64`, добавлен `SOC_EXYNOS5433` в `mach-exynos`.

## 2. Статус (2026-11-23, ночь)

Установлен **postmarketOS edge (Phosh, systemd)**: rootfs в USERDATA (mmcblk0p21,
подразделы pmOS_boot + pmOS_root), ядро `linux-samsung-treltexx` в BOOT.
Пакеты: `pmaports/device/testing/{device,linux}-samsung-treltexx`, сборка через
`tools/pmb` (отдельный конфиг `~/.config/pmbootstrap_note4.cfg`).

| Компонент | Статус |
|---|---|
| Загрузка с S-Boot, 8 ядер (своё SMP), 3 ГБ RAM | ✅ |
| eMMC HS400, pmOS rootfs, SSH по USB (172.16.42.1, user/password chosen at install) | ✅ (загружалось 02:48) |
| Дисплей DRM DECON→MIC→DSI→S6E3HA2, консоль | ✅ |
| Fuel gauge MAX17047 (`max170xx_battery`), заряд % | ✅ |
| Зарядка `max77843-charger` (1.5 A) | ✅ драйвер свой |
| Зависание шины PMIC (INTB от CHGR/FG без обработчика) | 🔧 исправлено в r5, не проверено |
| Тачскрин STM FTS | 🔧 hsync панели (`BD 11`) + reinit; события ещё не видели |
| Тач-кнопки Cypress (recent/back) | 🔧 DTS+драйвер добавлены (r6) |
| Звук WM5110 + MAX98504 | 🔧 конфиг+DTS (r6), не проверено |
| greetd (вход в Phosh) | ❌ падает — разобрать по SSH |
| Wi-Fi BCM4358 PCIe | ⬜ нужна nvram с SYSTEM |
| GPU Mali-T760 panfrost | ⬜ |

**Утром:** телефон завис (до фикса r5). Нужно: VolDown+Power 10 с →
VolDown+Home+Power → VolUp (Download), затем `heimdall flash --BOOT out/pmos-boot.img`.

### Грабли, на которые уже наступили
9. Конфиг ядра = linux-postmarketos-exynos5 + `kernel/note4.config` (merge_config.sh). Чистый allnoconfig ломал pmOS: нет COMPAT_32BIT_TIME (musl → ENOSYS), SYSCTL, BLK_DEV/loop.
10. pmOS dtbTool-exynos хэширует model_info — S-Boot Note 4 ждёт фиксированные коды; dt.img собирает `mkdtbh.py` в пакете ядра.
11. BOOT 14 МиБ → `deviceinfo_create_initfs_extra`. Ядро ≤ ~22 МБ, иначе распаковщик заедет на DTB @0x21f00000.
12. heimdall рвётся на USERDATA → rootfs пишется через OrangeFox `adb exec-in dd` (32-битный dd не умеет seek > 2 ГБ).
13. pmOS initramfs debug: telnet 172.16.42.1:23, `tools/n4tel`; файлы туда — `nc -l -p 5555` в foreground.
1. S-Boot кладёт ядро @0x20008000, DTB @0x21f00000, initrd @0x22000000 →
   Image+zImage должны влезть в ~31 МБ (multi_v7 не влез). Конфиг: `kernel/note4.config`.
2. S-Boot подменяет bootargs → `CONFIG_CMDLINE_FORCE`.
3. S-Boot переписывает `/memory reg` в 1-cell формате → `linux,usable-memory`.
4. Alpine busybox — Thumb-2 → нужен `CONFIG_ARM_THUMB`.
5. Панель командного режима: simplefb на 0x67000000 бесполезен (кадр не обновляется).
6. Fbdev 1440×2560 требует CMA (128 МБ).
7. ModemManager лезет в ttyACM — udev-правило `/etc/udev/rules.d/70-note4-debug.rules`.
8. busybox ash ждёт ответ на `ESC[6n` — `tools/n4sh` его шлёт.

### Цикл разработки
```
./build.sh kernel && ./build.sh img        # ~1 мин инкрементально
tools/n4sh 'rb download'                  # телефон сам уходит в Download
heimdall flash --BOOT out/boot.img        # грузится сам
tools/n4sh 'dmesg | tail'
```
Если ядро не дошло до userspace: VolDown+Power 10 с → VolUp+Home+Power (OrangeFox),
`adb shell cat /proc/last_kmsg` — там маркеры `note4: [N]` и весь printk.

## 3. Структура

```
build.sh                 сборка: ./build.sh [all|kernel|initramfs|img]
dts/exynos5433-treltexx.dts   наш DTS (симлинк в build/kernel/arch/arm/boot/dts/samsung/)
dts/mainline, dts/downstream  справочные копии
initramfs/               init + busybox (Alpine armv7 busybox-static 1.37)
tools/                   dtbhtool, mkdtbhbootimg, unpack* (из LineageOS hardware/samsung)
ref/                     BoardConfig, downstream dtsi/defconfig, samsung_dtbh.h
patches/                 патчи к ядру
build/kernel             linux-7.3.5 (+git для диффов)
out/                     boot.img, boot.tar.md5, dt.img, zImage
```

## 4. Параметры boot.img (из LineageOS trelte-common)

- base `0x10000000`, pagesize `2048`, ramdisk_offset `0x01000000`, tags_offset `0x100`
- DT отдельно, формат **DTBH** v2: platform `trlte` (код `0x3ec3`), subtype
  `trlte_eur_open` (код `0x3a37307e`). В DTS нужны `model_info-*`.
- cmdline: `console=tty0 nosmp clk_ignore_unused pd_ignore_unused loglevel=8 rdinit=/init`

## 5. Сборка

Тулчейн: clang/LLVM (32-битного gcc в системе нет) — `ARCH=arm LLVM=1`.
```
./build.sh          # ядро + initramfs + out/boot.img + out/boot.tar.md5
```

## 6. Прошивка и отладка

Download Mode: VolDown + Home + Power, затем VolUp.
```
heimdall flash --BOOT out/boot.img        # или Odin: AP = out/boot.tar.md5
```
Откат — стоковый/Lineage boot.img тем же способом или через OrangeFox.

После загрузки на ноуте появится USB-сеть (cdc_ncm):
```
ip addr add 172.16.42.2/24 dev <usbX>; telnet 172.16.42.1
```
Ещё есть `/dev/ttyACM0` (ACM-консоль).

Что значит поведение аппарата:
- висит на лого Samsung без перезагрузки → ядро не стартовало или умерло рано;
- перезагружается через ~30 с → вероятно, сторожевой таймер (значит, ядро живо хотя бы частично);
- синий/красный экран «Upload mode» → паника, пойманная sec_debug;
- появилось USB-устройство `18d1:d001` → userspace жив.

## 7. Ограничения

- Модем (Ericsson M7450 через LLI) — нет.
- Сенсорный хаб BCM4773, S-Pen (Wacom), камеры — пока нет.
- GPU Mali-T760 — **panfrost поддерживает** (T760 = RK3288), должно заработать.

## 8. TODO

1. Первая загрузка, проверить USB-сеть.
2. Снять адрес фреймбуфера (регистр DECON `0x13800000+…` через OrangeFox/devmem) → simplefb.
3. ramoops в RAM, которую S-Boot не трогает, — логи после ребута.
4. SMP: посмотреть `arch/arm/mach-exynos/platsmp.c` в downstream (SMC CPU boot).
5. Сверить тачклавиши (на TM2 — Cypress `tm2-touchkey`), S-Pen, Wi-Fi BCM4358 firmware.
