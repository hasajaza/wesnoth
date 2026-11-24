#!/bin/sh
# Wesnoth Access automated tests.
#
#   run_access_tests.sh <build dir>
#
# <build dir> contains the "wesnoth" and "boost_unit_tests" binaries built from
# this source tree. Runs, without any human interaction:
#   1. the C++ unit tests of the accessibility layer (Boost.Test),
#   2. every end-to-end script in utils/access/tests/*.script: the real game
#      is started with SDL's dummy video driver, the script injects keyboard
#      events and checks the text the accessibility layer speaks,
#   3. a regression check that the game still runs a WML unit test with the
#      accessibility layer disabled.
# Results are written to <build dir>/access-test-output/.
set -u

BIN=$(cd "$1" && pwd)
SRC=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$BIN/access-test-output"
rm -rf "$OUT"
mkdir -p "$OUT"
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
# The dummy video driver needs the software renderer.
export SDL_RENDER_DRIVER=software

failures=0
summary=""

note() {
	summary="$summary
$1"
	echo "$1"
}

# 1. Unit tests -------------------------------------------------------------
if [ -x "$BIN/boost_unit_tests" ]; then
	# The unit tests load game data relative to the source tree.
	if (cd "$SRC" && "$BIN/boost_unit_tests" --run_test=test_accessibility --log_level=test_suite > "$OUT/unit-tests.log" 2>&1); then
		note "PASS unit tests (test_accessibility)"
	else
		note "FAIL unit tests (test_accessibility), see unit-tests.log"
		failures=$((failures + 1))
	fi
else
	note "SKIP unit tests: boost_unit_tests not built"
fi

# 2. End-to-end scripts -----------------------------------------------------
for script in "$SRC"/utils/access/tests/*.script; do
	[ -f "$script" ] || continue
	name=$(basename "$script" .script)
	args=$(sed -n 's/^#ARGS:[ ]*//p' "$script")
	userdir="$OUT/$name-userdata"
	mkdir -p "$userdir"
	# shellcheck disable=SC2086
	timeout 900 "$BIN/wesnoth" --data-dir="$SRC" --userdata-dir="$userdir" --nosound --nomusic --noaddons \
		--resolution=1280x800 --windowed $args \
		--access-test-script="$script" --access-test-report="$OUT/$name.report" \
		--access-speech=log --access-speech-log="$OUT/$name.speech.log" \
		> "$OUT/$name.stdout.log" 2>&1
	rc=$?
	cp "$userdir"/logs/wesnoth-access.log "$OUT/$name.access.log" 2>/dev/null || true
	if [ $rc -eq 0 ] && grep -q "SUMMARY: .* 0 failed" "$OUT/$name.report" 2>/dev/null; then
		note "PASS end-to-end $name ($(grep SUMMARY "$OUT/$name.report"))"
	else
		note "FAIL end-to-end $name (exit code $rc; $(grep SUMMARY "$OUT/$name.report" 2>/dev/null || echo 'no report'))"
		grep "^FAIL" "$OUT/$name.report" 2>/dev/null | head -20
		failures=$((failures + 1))
	fi
done

# 3. Upstream WML unit test suite, with the layer disabled and enabled -------
# The complete upstream suite (wml_test_schedule) must give exactly the same
# results as for the unmodified game, both with accessibility disabled and
# with accessibility enabled (speech discarded), which shows that the
# read-only hooks do not change game behaviour or consume random numbers.
for mode in disabled enabled; do
	if [ "$mode" = disabled ]; then
		extra="-a=--no-accessibility"
	else
		extra="-a=--accessibility -a=--access-speech=none"
	fi
	# shellcheck disable=SC2086
	(cd "$SRC" && timeout 3000 python3 ./run_wml_tests -c -t 30 -bt 1000 -p "$BIN/wesnoth" \
		-a=--userdata-dir="$OUT/wml-$mode-userdata" $extra) > "$OUT/wml-tests-$mode.log" 2>&1
	rc=$?
	if [ $rc -eq 0 ]; then
		note "PASS upstream WML unit tests with accessibility $mode"
	else
		note "FAIL upstream WML unit tests with accessibility $mode (exit code $rc), see wml-tests-$mode.log"
		tail -n 30 "$OUT/wml-tests-$mode.log"
		failures=$((failures + 1))
	fi
done

echo "$summary" > "$OUT/SUMMARY.txt"
echo
echo "Wesnoth Access tests finished with $failures failing group(s)."
exit $failures
