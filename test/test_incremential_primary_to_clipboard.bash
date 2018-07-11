#!/usr/bin/bash

# this test requires the MAX_INCR define to be less than 11 just to make xcsyncd
# transfer the clipboard data in inctemential maner.

DAEMON=$1
timeout=0
SIG=15		# SIGTERM

if [[ ${DAEMON} == "" ]]
then
	echo "usage: $0 <daemon_executable_name>"
	exit 1
fi

./${DAEMON} &> /dev/null &
DAEMON_PID=$!

string="0123456789012345678901234"
xclip -in -selection primary <<< "$string"
timeout 1 xclip -out -selection clipboard
if [[ $? == 124 ]]
then
	timeout=1
fi

pkill -${SIG} ${DAEMON} &
wait ${DAEMON_PID}
RESULT=$?

let SIG+=128
if [[ ${timeout} == 1 ]]
then
	echo "Test failed. xclip killed when time went out."
	exit 124;
elif  [[ ${RESULT} != ${SIG} ]]
then
	echo "Test failed. exit result is: ${RESULT}"
	exit ${RESULT}
else
	exit 0
fi
