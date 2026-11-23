#!/bin/sh
# Rebuild iio-sensor-proxy 3.9 with rotation thresholds 35 -> 60 degrees
# (source with patched src/orientation.c is expected at /tmp/isp.tgz).
set -e
apk add -q build-base meson glib-dev libgudev-dev polkit-dev
rm -rf /root/isp && mkdir /root/isp && cd /root/isp && tar xzf /tmp/isp.tgz
cd iio-sensor-proxy-3.9
grep -n "THRESHOLD_\(LANDSCAPE\|PORTRAIT\)  60" src/orientation.c
meson setup build --prefix=/usr/local -Dtests=false -Dgtk-tests=false -Dgtk_doc=false -Dssc-support=disabled \
	-Dudevrulesdir=/usr/local/lib/udev -Dsystemdsystemunitdir=/usr/local/lib/systemd
ninja -C build
install -m755 build/src/iio-sensor-proxy /usr/local/libexec-iio-sensor-proxy
mkdir -p /etc/systemd/system/iio-sensor-proxy.service.d
printf '[Service]\nExecStart=\nExecStart=/usr/local/libexec-iio-sensor-proxy\n' > /etc/systemd/system/iio-sensor-proxy.service.d/threshold.conf
systemctl daemon-reload
systemctl restart iio-sensor-proxy
echo BUILD-OK
