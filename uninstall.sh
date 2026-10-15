MODDIR=${0%/*}
[ -f "$MODDIR/sensit.pid" ] && kill "$(cat "$MODDIR/sensit.pid")" 2>/dev/null
