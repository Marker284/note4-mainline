#!/usr/bin/env bash
# Regenerate the Note 4 kernel config:
#   pmOS exynos5 config + kernel/note4.config
#   -> make localmodconfig (modules actually used on the phone: kernel/lsmod-note4.txt)
#   -> + kernel/note4.config + kernel/note4-keep.config (on-demand modules)
# Result: build/kernel/.config and the linux-samsung-treltexx package config.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
K=$P/build/kernel
export ARCH=arm LLVM=1
cd "$K"
cp "$P/pmaports/device/community/linux-postmarketos-exynos5/config-postmarketos-exynos5.armv7" .config
scripts/kconfig/merge_config.sh -m .config "$P/kernel/note4.config" >/dev/null
make -s olddefconfig >/dev/null 2>&1
LSMOD="$P/kernel/lsmod-note4.txt" make localmodconfig </dev/null >/dev/null 2>&1
scripts/kconfig/merge_config.sh -m .config "$P/kernel/note4.config" "$P/kernel/note4-keep.config" >/dev/null
make -s olddefconfig >/dev/null 2>&1
cp .config "$P/pmaports/device/testing/linux-samsung-treltexx/config-samsung-treltexx.armv7"
echo "config: $(grep -c '=m' .config) modules, $(grep -c '=y' .config) built-in"
