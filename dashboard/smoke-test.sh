#!/usr/bin/env bash
# Cadence API smoke test. Run against a local dev server or the Vercel URL.
#
#   BASE=http://localhost:3000 TOKEN=your_token ./smoke-test.sh
#
# Every check below has failed in a real project at least once. Run the whole
# file after any change to the routes.

set -u
BASE="${BASE:-http://localhost:3000}"
TOKEN="${TOKEN:?set TOKEN to the value of DEVICE_TOKEN}"
DEV="cadence-hub-01"
AUTH="Authorization: Bearer ${TOKEN}"
JSON="Content-Type: application/json"

pass=0; fail=0
check() {  # check <label> <expected-status> <actual-status> <body>
  if [ "$2" = "$3" ]; then
    printf '  ok    %-46s %s\n' "$1" "$3"; pass=$((pass+1))
  else
    printf '  FAIL  %-46s got %s want %s\n        %s\n' "$1" "$3" "$2" "$4"; fail=$((fail+1))
  fi
}
call() {   # call <method> <path> <data> -> sets STATUS and BODY
  local out
  if [ -n "$3" ]; then
    out=$(curl -sS -w '\n%{http_code}' -X "$1" "${BASE}$2" -H "$AUTH" -H "$JSON" -d "$3")
  else
    out=$(curl -sS -w '\n%{http_code}' -X "$1" "${BASE}$2" -H "$AUTH")
  fi
  STATUS="${out##*$'\n'}"; BODY="${out%$'\n'*}"
}

NOW=$(date -u +%Y-%m-%dT%H:%M:%SZ)
T1=$(date -u -d '-40 min' +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u -v-40M +%Y-%m-%dT%H:%M:%SZ)
T2=$(date -u -d '-10 min' +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date -u -v-10M +%Y-%m-%dT%H:%M:%SZ)
TODAY=$(date -u +%Y-%m-%d)

echo "== auth =="
out=$(curl -sS -w '\n%{http_code}' -X POST "${BASE}/api/ingest/readings" \
      -H "$JSON" -d '{"device_id":"x","ts":"'"$NOW"'"}')
check "no token is rejected" 401 "${out##*$'\n'}" "${out%$'\n'*}"

out=$(curl -sS -w '\n%{http_code}' -X POST "${BASE}/api/ingest/readings" \
      -H "Authorization: Bearer wrong" -H "$JSON" -d '{"device_id":"x","ts":"'"$NOW"'"}')
check "wrong token is rejected" 401 "${out##*$'\n'}" "${out%$'\n'*}"

echo "== readings =="
call POST /api/ingest/readings \
  '{"device_id":"'"$DEV"'","ts":"'"$NOW"'","temp_c":29.4,"humidity":71,"pressure_hpa":1006,"lux":180,"real_feel_c":34.1}'
check "valid reading accepted" 201 "$STATUS" "$BODY"

call POST /api/ingest/readings \
  '{"device_id":"'"$DEV"'","ts":"'"$NOW"'","temp_c":29.4,"humidity":71,"pressure_hpa":1006,"lux":180,"real_feel_c":34.1}'
check "duplicate reading is idempotent" 201 "$STATUS" "$BODY"

call POST /api/ingest/readings \
  '{"device_id":"'"$DEV"'","ts":"'"$NOW"'","temp_c":null,"humidity":null,"pressure_hpa":null,"lux":210}'
check "null sensor fields allowed (BME absent)" 201 "$STATUS" "$BODY"

call POST /api/ingest/readings \
  '{"device_id":"'"$DEV"'","ts":"'"$NOW"'","temp_c":250}'
check "out-of-range temp rejected" 400 "$STATUS" "$BODY"

call POST /api/ingest/readings '{"device_id":"'"$DEV"'","ts":"1999-01-01T00:00:00Z"}'
check "stale timestamp rejected (NTP guard)" 400 "$STATUS" "$BODY"

echo "== session events =="
call POST /api/ingest/session-events \
  '{"device_id":"'"$DEV"'","ts":"'"$T1"'","type":"start","source":"button"}'
check "single event accepted" 201 "$STATUS" "$BODY"

call POST /api/ingest/session-events \
  '{"device_id":"'"$DEV"'","events":[{"ts":"'"$T2"'","type":"stop","source":"button"}]}'
check "batched event accepted" 201 "$STATUS" "$BODY"

call POST /api/ingest/session-events \
  '{"device_id":"'"$DEV"'","ts":"'"$T1"'","type":"start","source":"button"}'
check "replayed event is idempotent" 201 "$STATUS" "$BODY"

call POST /api/ingest/session-events \
  '{"device_id":"'"$DEV"'","ts":"'"$NOW"'","type":"nonsense","source":"button"}'
check "bad event type rejected" 400 "$STATUS" "$BODY"

echo "== todos =="
call POST /api/todos '{"title":"Solder the encoder caps"}'
check "todo created" 201 "$STATUS" "$BODY"
ID=$(printf '%s' "$BODY" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')

call GET /api/todos ''
check "todo list readable without token" 200 "$STATUS" "$BODY"

call PATCH /api/todos '{"id":"'"$ID"'","done":true}'
check "todo marked done" 200 "$STATUS" "$BODY"

call GET /api/todos ''
if printf '%s' "$BODY" | grep -q "$ID"; then
  printf '  FAIL  %-46s done item still listed\n' "done items excluded from GET"; fail=$((fail+1))
else
  printf '  ok    %-46s\n' "done items excluded from GET"; pass=$((pass+1))
fi

call DELETE "/api/todos?id=${ID}" ''
check "todo deleted" 200 "$STATUS" "$BODY"

echo "== stats =="
call GET "/api/stats/daily?date=${TODAY}" ''
check "daily stats for one date" 200 "$STATUS" "$BODY"
echo "        $BODY"

call GET "/api/stats/daily?from=${TODAY}&to=${TODAY}" ''
check "daily stats for a range" 200 "$STATUS" "$BODY"

call GET "/api/stats/daily?date=garbage" ''
check "malformed date rejected" 400 "$STATUS" "$BODY"

echo
echo "  ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ] || exit 1