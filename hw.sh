#!/system/bin/sh
# called by the daemon: hw.sh 1|0
# 1 = hw touch boost on (setprop debug.sf.hw 1), the old value is kept in hw.prev
# 0 = restore the old value, safe to call any time (does nothing when nothing was changed)
MODDIR=${0%/*}
P=debug.sf.hw
F=$MODDIR/hw.prev
if [ "$1" = 1 ]; then
  [ -f "$F" ] || getprop "$P" > "$F"
  setprop "$P" 1
elif [ -f "$F" ]; then
  setprop "$P" "$(cat "$F")"
  rm -f "$F"
fi
