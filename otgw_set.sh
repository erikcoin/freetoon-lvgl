#!/bin/sh
# otgw_set.sh — toggle / set an OTGW setting via the firmware's HTTP API.
#
# Usage:
#   ./otgw_set.sh [-h HOST] <name> <value>
#   ./otgw_set.sh [-h HOST] --list           # dump all settings
#   ./otgw_set.sh [-h HOST] --get NAME       # show one setting
#   ./otgw_set.sh [-h HOST] --cmd "OT=..."   # send a raw OTGW command
#
# Default host: 192.168.99.21
# Auth: none — the API isn't password-protected, just CSRF-free.
#
# Notes:
#   - Send commands requires the otgwcommandenable setting to be "true".
#     Toggle it once with: ./otgw_set.sh otgwcommandenable true
#   - boolean values are "true" / "false" (strings). integers as decimal.
#   - Bypass any VPN that's routing 192.168.99.0/24 — see OPNsense docs.

set -e
HOST="192.168.99.21"

# parse --host / -h
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--host) HOST="$2"; shift 2 ;;
    *) break ;;
  esac
done

case "$1" in
  --list)
    curl -s --max-time 5 "http://$HOST/api/v0/settings" ;;
  --get)
    curl -s --max-time 5 "http://$HOST/api/v0/settings" \
      | grep -oE "\"name\": ?\"$2\"[^}]*" || echo "  (not found)"
    echo ;;
  --cmd)
    curl -s --max-time 5 -X POST \
      -H "Content-Type: application/json; charset=UTF-8" \
      -d "{\"command\":\"$2\"}" "http://$HOST/api/v0/otgw/command" ;;
  --help|"")
    sed -n '2,17p' "$0" ;;
  *)
    NAME="$1"; VALUE="$2"
    if [ -z "$VALUE" ]; then
      echo "usage: $0 <name> <value>" >&2; exit 1
    fi
    curl -s --max-time 5 -X POST \
      -H "Content-Type: application/json; charset=UTF-8" \
      -d "{\"name\":\"$NAME\",\"value\":\"$VALUE\"}" \
      "http://$HOST/api/v0/settings"
    echo
    echo "--- verify ---"
    "$0" -h "$HOST" --get "$NAME" ;;
esac
