#!/usr/bin/bash

DAEMON=xcsyncd
SIG=15		# SIGTERM

./${DAEMON} & &> /dev/null
DAEMON_PID=$!

string=${RANDOM}
xclip -i -selection primary <<< "$string"
timeout 1 xclip -o -selection clipboard

pkill -${SIG} ${DAEMON} &
wait ${DAEMON_PID}
RESULT=$?

let SIG+=128
if [[ ${RESULT} == ${SIG} ]]
then
	exit 0
else
	echo "Test failed. exit result is: ${RESULT}"
	exit ${RESULT}
fi
