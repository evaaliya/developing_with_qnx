#!/bin/bash
# Host check of the panel buttons against the live network on the guest.
# Each button is the same POST the page sends. The mode is read back from
# the panel's /text, which shows telemetry, not the button that was pressed.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PAGE="http://127.0.0.1:8765"
SSH=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes root@localhost)
LOG="$ROOT/build/panel-check.log"
failures=0

say() { echo "panel-check: $*"; }
bad() { echo "panel-check: FAIL $*"; failures=$((failures + 1)); }

press() {
    curl -s -o /tmp/panel-check-body.$$ -w '%{http_code}' -X POST \
        -H 'X-Panel-Command: 1' -H "Origin: $PAGE" \
        --data "name=$1" "$PAGE/command"
}

wait_mode() {
    local want=$1 i
    for i in $(seq 1 50); do
        if curl -s "$PAGE/text" | grep -q "^source mode: $want\$"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

mkdir -p "$ROOT/build"
rm -f "$LOG"
scp -P 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes \
    "$ROOT/scripts/qnx-telemetry.sh" root@localhost:/tmp/qnx-telemetry.sh >/dev/null || exit 1
"${SSH[@]}" 'chmod +x /tmp/qnx-telemetry.sh /tmp/source /tmp/sink /tmp/relay /tmp/supervisor /tmp/telemetry; /tmp/qnx-telemetry.sh' \
    > "$ROOT/build/panel-check-guest.log" 2>&1 &
guest=$!

/usr/bin/ruby "$ROOT/scripts/host-panel.rb" --no-browser --log="$LOG" \
    > "$ROOT/build/panel-check-panel.log" 2>&1 &
panel=$!
trap 'kill $panel 2>/dev/null; "${SSH[@]}" "touch /tmp/telemetry-done" >/dev/null 2>&1; rm -f /tmp/panel-check-body.$$' EXIT

if wait_mode normal || { sleep 5; wait_mode normal; }; then
    say "telemetry shows mode normal"
else
    bad "the panel never showed mode normal"
fi

curl -s "$PAGE/" | grep -o 'data-command="[a-z-]*"' | wc -l | tr -d ' ' | sed 's/^/panel-check: buttons on the page /'

for step in "flood-on flood" "flood-off normal" "mute mute" "restore normal"; do
    set -- $step
    code=$(press "$1")
    body=$(cat /tmp/panel-check-body.$$)
    say "pressed $1: HTTP $code, $body"
    [ "$code" = 200 ] || bad "$1 returned $code"
    if wait_mode "$2"; then
        say "telemetry shows mode $2 after $1"
    else
        bad "telemetry did not show mode $2 after $1"
    fi
done

code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data 'name=flood-on' "$PAGE/command")
say "POST without the panel header: HTTP $code"
[ "$code" = 403 ] || bad "a POST without the header was not refused"
code=$(press stop)
say "stop: HTTP $code, $(cat /tmp/panel-check-body.$$)"
[ "$code" = 400 ] || bad "stop was not refused"

"${SSH[@]}" 'touch /tmp/telemetry-done' >/dev/null 2>&1
wait "$guest"
guest_status=$?
say "guest network exit $guest_status"
grep -E '^sink: (summary|unaccounted)' "$ROOT/build/panel-check-guest.log" | sed 's/^/panel-check: /'
[ "$guest_status" = 0 ] || bad "the guest network exited $guest_status"
grep -q 'sink: unaccounted 0' "$ROOT/build/panel-check-guest.log" || bad "unaccounted is not 0"
grep 'panel: command' "$ROOT/build/panel-check-panel.log" | sed 's/^/panel-check: /'

if [ "$failures" -eq 0 ]; then
    say PASS
    exit 0
fi
say "$failures failure(s)"
exit 1
