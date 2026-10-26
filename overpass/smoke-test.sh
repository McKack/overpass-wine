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
# Without Wine Mono and Gecko in Wine's download cache, setting up a prefix offers to download them in a
# dialog that waits for a click; the test needs neither (OverPass caches both before a bottle's first boot).
export WINEDLLOVERRIDES="mscoree,mshtml="
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
        echo "Wine processes still running:"
        ps -axo pid,etime,command | grep -iE 'wine|\.exe' | grep -v grep || true
        "$W/bin/wineserver" -k 2>/dev/null || true
        exit 1
    fi
}

# Packaging: GStreamer and libinotify are in, and nothing they need is missing from lib/.
GST_SO="$W/lib/wine/x86_64-unix/winegstreamer.so"
[ -f "$GST_SO" ] || { echo "::error::winegstreamer.so wasn't built"; exit 1; }
otool -L "$W/bin/wineserver" | grep -q libinotify || { echo "::error::wineserver isn't linked with libinotify"; exit 1; }
for f in "$GST_SO" "$W/bin/wineserver" "$W"/lib/*.dylib "$W"/lib/gstreamer-1.0/*.dylib; do
    for dep in $(otool -L "$f" | tail -n +2 | awk '{print $1}' | grep '^@rpath/'); do
        name="${dep#@rpath/}"
        [ -f "$W/lib/$name" ] || { echo "::error::$(basename "$f") needs $name, not in lib/"; exit 1; }
    done
done
echo "GStreamer plugins: $(ls "$W"/lib/gstreamer-1.0 | wc -l | tr -d ' ')"

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
