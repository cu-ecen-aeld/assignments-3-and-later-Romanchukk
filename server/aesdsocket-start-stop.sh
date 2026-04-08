#!/bin/sh

PROGRAM_NAME=aesdsocket
PROGRAM_PATH=/usr/bin/${PROGRAM_NAME}
PROGRAM_ARGS="-d"

case $1 in
	start)
		echo "Start ${PROGRAM_NAME}"
		start-stop-daemon -S -n ${PROGRAM_NAME} -a ${PROGRAM_PATH} -- ${PROGRAM_ARGS}
	;;
	stop)
		echo "Stop ${PROGRAM_NAME}"
		start-stop-daemon -K -n ${PROGRAM_NAME}
	;;
	*)
		echo "Failed to start ${PROGRAM_NAME}! Usage: $0 {start|stop}"
		exit 1
esac
