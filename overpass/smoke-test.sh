#!/bin/bash
# Checks a built OverPass Wine: a prefix boots, winebus.sys (controllers) survives loading SDL, and the
# compatibility rules (patch 3) reach processes created from Windows code.
#
#   overpass/smoke-test.sh <unpacked overpass-wine-* folder>
#
# Every Wine command has a time limit, so a hang fails the test with its log instead of using up the job.
set -euo pipefail

W="$(cd "$1" && pwd)"
TMP="${RUNNER_TEMP:-$(mktemp -d)}"
export WINEPREFIX="$TMP/prefix" WINEDEBUG="${WINEDEBUG:--all}"
rm -rf "$WINEPREFIX"

# limit SECONDS LOG COMMAND...: runs COMMAND with stdout appended to LOG and stderr to LOG.err; when it
# fails or runs out of time, prints both and fails.
limit() {
    local seconds=$1 log=$2
    shift 2
    if ! perl -e 'alarm shift; exec @ARGV or die' "$seconds" "$@" >>"$log" 2>>"$log.err"; then
        echo "::error::'$*' failed or took longer than ${seconds}s; last log lines:"
        tail -50 "$log"
        tail -200 "$log.err"
        "$W/bin/wineserver" -k 2>/dev/null || true
        exit 1
    fi
}

"$W/bin/wine" --version

# winebus.sys traces (channel hid) show how far SDL got if booting hangs.
WINEDEBUG=-all,+hid limit 600 "$TMP/boot.log" "$W/bin/wine" wineboot --init
limit 120 "$TMP/boot.log" "$W/bin/wine" cmd /c ver
limit 120 "$TMP/boot.log" "$W/bin/wineserver" -w
# A broken SDL aborts winebus.sys with an NSException.
if grep -q "uncaught exception" "$TMP/boot.log.err"; then echo "::error::winebus.sys crashed"; tail -100 "$TMP/boot.log.err"; exit 1; fi
grep -m1 'L"SDL" main loop started' "$TMP/boot.log.err" || echo "::warning::SDL bus didn't start (no SDL or no controllers support)"

mkdir -p "$WINEPREFIX/drive_c/rules"
export OVERPASS_COMPAT_RULES='C:\rules'

# env reaches a child cmd.exe.
printf '# smoke test\n[cmd test]\nprogram = cmd.exe\nenv = OVERPASS_RULE_TEST=applied\n' > "$WINEPREFIX/drive_c/rules/test.ini"
limit 120 "$TMP/rule.log" "$W/bin/wine" cmd /c cmd /v:on /c echo !OVERPASS_RULE_TEST!
out="$(tr -d '\r' < "$TMP/rule.log" | tail -1)"
echo "rule test: $out"
[ "$out" = "applied" ]

# replace-path starts another program with the same arguments: hostname.exe becomes cmd.exe here.
printf '[replace test]\nprogram = hostname.exe\nreplace-path = \\hostname.exe => \\cmd.exe\n' > "$WINEPREFIX/drive_c/rules/replace.ini"
limit 120 "$TMP/replace.log" "$W/bin/wine" cmd /c hostname.exe /c echo replaced
out="$(tr -d '\r' < "$TMP/replace.log" | tail -1)"
echo "replace test: $out"
[ "$out" = "replaced" ]

nm -gU "$W/lib/wine/x86_64-unix/winemac.so" | grep -q _macdrv_functions && echo "macdrv_functions exported"
"$W/bin/wineserver" -k 2>/dev/null || true
echo "smoke test passed"
