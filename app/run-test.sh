#!/bin/sh
#
# Copyright (c) 2020 Peter Johanson; Cody McGinnis
#
# SPDX-License-Identifier: MIT
#

if [ -z "$1" ]; then
	echo "Usage: ./run-test.sh <path to testcase>"
	exit 1
elif [ "$1" = "all" ]; then
	echo "" > ./build/tests/pass-fail.log
	find tests -name native_posix.keymap -exec dirname \{\} \; | xargs -l -P 4 ./run-test.sh > ./build/tests/pass-fail.log
	err=$?
	sort -k2 ./build/tests/pass-fail.log
	exit $err
fi

testcase="$1"
echo "Running $testcase:"

west build -d build/$testcase -b native_posix --pristine -- -DZMK_CONFIG=$testcase > build/$testcase/build.log #2>&1
if [ $? -gt 0 ]; then
	echo "FAIL: $testcase did not build"
else
	./build/$testcase/zephyr/zmk.exe | sed -e "s/.*> //" | tee build/$testcase/keycode_events_full.log | sed -n -f $testcase/events.patterns > build/$testcase/keycode_events.log
	diff -au $testcase/keycode_events.snapshot build/$testcase/keycode_events.log
	if [ $? -gt 0 ]; then
		echo "FAIL: $testcase"
	else
		echo "PASS: $testcase"
	fi
fi