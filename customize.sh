# pick the binary for this phone: arm64 or arm (32-bit), anything else is unsupported
A=$ARCH
case "$A" in arm64|arm) ;; *)
  case "$(getprop ro.product.cpu.abi)" in
    arm64*) A=arm64 ;;
    armeabi*) A=arm ;;
    *) abort "Only arm64 and arm (32-bit) are supported" ;;
  esac
esac
BIN=$MODPATH/system/bin
[ -f "$BIN/flick.$A" ] || abort "Binary for $A is missing"

# stop an older running instance, its binary cannot be overwritten while it runs.
# the second path is the old Vektor (id sensit): its daemon would still hold the touchscreen
PL=/data/user_de/0/com.android.shell/axeron/plugins
for F in "$PL/flick/flick.pid" "$PL/sensit/sensit.pid"; do
  [ -f "$F" ] && kill "$(cat "$F")" 2>/dev/null
done

mv -f "$BIN/flick.$A" "$BIN/flick"
rm -f "$BIN/flick.arm" "$BIN/flick.arm64"
chmod 755 "$BIN/flick"
ui_print "Flick installed ($A). Open its WebUI to use it."
