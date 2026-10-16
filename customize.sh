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
[ -f "$BIN/sensit.$A" ] || abort "Binary for $A is missing"

# stop an older running instance, its binary cannot be overwritten while it runs
OLD=/data/user_de/0/com.android.shell/axeron/plugins/sensit
[ -f "$OLD/sensit.pid" ] && kill "$(cat "$OLD/sensit.pid")" 2>/dev/null
rm -f "$OLD/notify.sh" "$OLD/toast.sh"  # the toast now comes from the WebUI

mv -f "$BIN/sensit.$A" "$BIN/sensit"
rm -f "$BIN/sensit.arm" "$BIN/sensit.arm64"
chmod 755 "$BIN/sensit"
ui_print "Vektor installed ($A). Open its WebUI to use it."
