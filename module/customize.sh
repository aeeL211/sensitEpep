[ "$ARCH" = "arm64" ] || abort "Only arm64 is supported"
OLD=/data/local/tmp/AxManager/plugins/sensit
[ -f "$OLD/sensit.pid" ] && kill "$(cat "$OLD/sensit.pid")" 2>/dev/null
chmod 755 "$MODPATH/system/bin/sensit"
ui_print "Sensitivity installed. Open its WebUI to use it."
