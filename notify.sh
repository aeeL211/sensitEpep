#!/system/bin/sh
# called by sensit when the boost turns on/off: notify.sh on|off <sens_h> <sens_v>
MODDIR=${0%/*}
if [ "$1" = "on" ]; then MSG="Sensitivity ON (H ${2}x, V ${3}x)"; else MSG="Sensitivity OFF"; fi

# custom toast hook: a toast.sh next to this file handles the message
[ -f "$MODDIR/toast.sh" ] && exec sh "$MODDIR/toast.sh" "$MSG"

# fallback: the shell has no toast command, so post a (replaceable) notification
exec /system/bin/cmd notification post -t Sensitivity sensit "$MSG" >/dev/null 2>&1
