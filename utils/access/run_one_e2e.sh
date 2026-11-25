#!/bin/sh
# Runs one end-to-end script inside the Linux test image (developer helper).
#   run_one_e2e.sh <name>   (utils/access/tests/<name>.script, binary in /output)
set -u
name="$1"
rm -rf /usr/local/lib/libboost* && ldconfig
ud="/output/e2e/ud-$name"
mkdir -p /output/e2e
find "$ud" -mindepth 1 -delete 2>/dev/null
args=$(grep '^#ARGS:' "/wesnoth/utils/access/tests/$name.script" | sed 's/^#ARGS://')
timeout 900 /output/wesnoth --data-dir=/wesnoth --userdata-dir="$ud" --nosound --nomusic --noaddons \
  --resolution=1280x800 --windowed $args --no-log-to-file \
  --access-test-script="/wesnoth/utils/access/tests/$name.script" \
  --access-test-report="/output/e2e/$name.report" --access-speech=log \
  --access-speech-log="/output/e2e/$name.speech.log" > /dev/null 2>&1
grep 'SUMMARY' "/output/e2e/$name.report"
