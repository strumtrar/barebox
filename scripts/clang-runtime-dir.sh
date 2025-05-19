#!/bin/sh

CC=${1:-${CC}}

if [ ${CC} = "" ]; then
	echo "Error: No compiler specified." >&2
	printf "Usage:\n\t$0 <clang-command>\n" >&2
	exit 1
fi

rundir=$(${CC} --print-runtime-dir)
if [ ! -e "$rundir" ]; then
	guess=$(dirname "$rundir")/linux
	if [ -e "$guess" ]; then
		rundir="$guess"
	fi
fi
echo "$rundir"
