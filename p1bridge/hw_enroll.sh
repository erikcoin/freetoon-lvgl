#!/bin/sh
# hw_enroll.sh — mint a v2 API token from a HomeWizard P1 device.
# Run on the Toon (or any host that can reach the HW IPs).
# Usage:  hw_enroll.sh <hw-ip> <name-for-our-user>
# Example: hw_enroll.sh 192.168.99.69  toon-p1bridge-elec
#
# Flow (HomeWizard API v2):
#   POST https://<ip>/api/user  body {"name":"<name>"}
#   - returns 403 while the WiFi-pair button has NOT been pressed
#   - returns 200 with {"token":"<token>"} once it has
#   You have ~30s to ask after pressing.
# We poll once per second for up to 60s and print the token when it arrives.

IP="$1"; RAW="${2:-toon-p1bridge}"
if [ -z "$IP" ]; then
  echo "usage: $0 <hw-ip> [name]"; exit 1
fi
# HomeWizard v2 requires the name to be prefixed with local/.
case "$RAW" in
  local/*) NAME="$RAW" ;;
  *)       NAME="local/$RAW" ;;
esac

echo "Probing v2 readiness on $IP ..."
PROBE=$(curl -k -s --max-time 3 -o /tmp/hw_probe.json -w '%{http_code}' \
             "https://$IP/api" 2>/dev/null)
BODY=$(cat /tmp/hw_probe.json 2>/dev/null)
echo "  http=$PROBE body=$BODY"
# v2 ready signal:
#   401 + "user:unauthorized"   = v2 on, no token (expected pre-enrolment)
#   200 + api_version:"v2"      = v2 on, with token-less /api endpoint
# anything else = no v2 listener
case "$PROBE/$BODY" in
  401/*user:unauthorized*) : ;;
  200/*api_version*v2*) : ;;
  200/*api_version*\"2*) : ;;
  *) echo "  device does not look v2-ready (https://$IP/api responded $PROBE)."
     exit 3 ;;
esac
rm -f /tmp/hw_probe.json

echo
echo "Press and hold the WiFi/pair button on the device for 1-3 seconds NOW."
echo "Polling POST /api/user every 1s (timeout 60s)..."

for i in $(seq 1 60); do
  RESP=$(curl -k -s -o /tmp/hw_resp.json -w '%{http_code}' \
              -X POST "https://$IP/api/user" \
              -H 'Content-Type: application/json' \
              --data "{\"name\":\"$NAME\"}" 2>/dev/null)
  case "$RESP" in
    200)
      # response is either {"token":"..."} or {"user":"..."} depending on fw
      TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /tmp/hw_resp.json)
      [ -z "$TOKEN" ] && TOKEN=$(sed -n 's/.*"user":"\([^"]*\)".*/\1/p' /tmp/hw_resp.json)
      echo
      echo "OK — token for $IP ($NAME):"
      echo "  $TOKEN"
      echo "  (raw response: $(cat /tmp/hw_resp.json))"
      echo
      echo "Add to /mnt/data/p1bridge.conf  (one line per device):"
      echo "  $IP=$TOKEN"
      rm -f /tmp/hw_resp.json
      exit 0
      ;;
    403) printf "." ;;     # waiting for button press
    *)   printf "[%s]" "$RESP" ;;
  esac
  sleep 1
done

echo
echo "Timed out without a token. Retry: re-press the button and rerun."
exit 4
