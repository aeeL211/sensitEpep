#!/system/bin/sh
# Called by sensit when the feature turns on/off:  notify.sh on|off <sens_h> <sens_v>
MODDIR=${0%/*}
if [ "$1" = "on" ]; then MSG="Sensitivity ON (H ${2}x, V ${3}x)"; else MSG="Sensitivity OFF"; fi

# Custom toast hook: if a toast.sh exists next to this file it handles the message.
[ -f "$MODDIR/toast.sh" ] && exec sh "$MODDIR/toast.sh" "$MSG"

# Fallback: the shell has no built-in toast command, so post a (replaceable) notification.
exec /system/bin/cmd notification post -t Sensitivity sensit "$MSG" >/dev/null 2>&1
