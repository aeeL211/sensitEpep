MODDIR=${0%/*}
[ -f "$MODDIR/flick.pid" ] && kill "$(cat "$MODDIR/flick.pid")" 2>/dev/null
# give the daemon a moment to restore debug.sf.hw, then make sure it is restored
sleep 1
[ -f "$MODDIR/hw.sh" ] && sh "$MODDIR/hw.sh" 0
