#!/usr/bin/env bash
# Send a test notification to one Pushover device (e.g. a SkyDisplay), to check its notifications screen and alarm.
#
#   tools/pushover_test.sh [normal|high|emergency] [device]      (defaults: normal, skydisplay-2)
#
# The app token and user key come from PUSHOVER_TOKEN and PUSHOVER_USER, or are asked for (not echoed).
# Nothing is saved. Emergency messages repeat every 60 s for up to 10 minutes until acknowledged
# (on the display or anywhere else).
set -euo pipefail

level="${1:-normal}"
device="${2:-skydisplay-2}"
case "$level" in
  normal)    priority=0 ;;
  high)      priority=1 ;;
  emergency) priority=2 ;;
  *) echo "usage: $0 [normal|high|emergency] [device]" >&2; exit 2 ;;
esac

token="${PUSHOVER_TOKEN:-}"
user="${PUSHOVER_USER:-}"
[ -n "$token" ] || { read -rsp "Pushover app token: " token; echo; }
[ -n "$user" ]  || { read -rsp "Pushover user key: " user; echo; }

args=(--form-string "token=$token" --form-string "user=$user" --form-string "device=$device"
      --form-string "priority=$priority" --form-string "title=SkyDisplay test ($level)"
      --form-string "message=Test sent $(date '+%H:%M:%S') to $device at $level priority.")
if [ "$priority" = 2 ]; then args+=(--form-string "retry=60" --form-string "expire=600"); fi

curl -sS https://api.pushover.net/1/messages.json "${args[@]}"
echo
