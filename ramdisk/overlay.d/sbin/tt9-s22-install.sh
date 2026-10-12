#!/system/bin/sh
# Copyright (C) 2026 cat-s22
# SPDX-License-Identifier: GPL-2.0-only
# Installs the TT9 S22 keyboard bundled in this boot image, once per version,
# then deletes the bundled APK so it does not stay in RAM.
# If the user uninstalls it, it is not installed again (marker file), not even
# when a later image bundles a newer version.
dir=${0%/*}
apk=$dir/tt9-s22.apk
pkg=io.github.sspanak.tt9.s22
vc=1652
mark=/data/adb/tt9-s22.installed
log() { /system/bin/log -t tt9-s22 "$*"; }

old=$(cat $mark 2>/dev/null)
if [ "$old" -ge $vc ] 2>/dev/null; then
  rm -f "$apk"; exit 0
fi
cur=$(dumpsys package $pkg 2>/dev/null | sed -n "s/.*versionCode=\([0-9]*\).*/\1/p" | head -n 1)
# Installed by an older image and uninstalled since: it stays gone.
if [ -n "$old" ] && [ -z "$cur" ]; then
  rm -f "$apk"; exit 0
fi
if [ -z "$cur" ] || [ "$cur" -lt $vc ]; then
  tmp=/data/local/tmp/tt9-s22.apk
  cp "$apk" $tmp && chmod 644 $tmp
  if pm install -r $tmp >/dev/null 2>&1; then
    log "installed $vc (was ${cur:-none})"
    if [ -z "$cur" ]; then
      ime enable $pkg/io.github.sspanak.tt9.ime.TraditionalT9 >/dev/null 2>&1
      ime set $pkg/io.github.sspanak.tt9.ime.TraditionalT9 >/dev/null 2>&1
    fi
    echo $vc > $mark
  else
    log "install failed, will retry on next boot"
  fi
  rm -f $tmp
else
  echo $vc > $mark
fi
rm -f "$apk"
