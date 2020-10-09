#!/usr/bin/env bash
set -euxo pipefail

IDLE_SO="../src/libidle.so"

make -C ../src ${IDLE_SO}
rm .libidle_state
LD_PRELOAD=${LD_PRELOAD:+${LD_PRELOAD}:}${IDLE_SO} ./accept &
sleep 0.5
flock -x .libidle_state echo "Locked."
kill $!
test $(cat .libidle_state) == "1"
echo -e "- \e[32mTest successful."
