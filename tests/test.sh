#!/usr/bin/env bash
set -euxo pipefail

IDLE_SO="../src/libidle.so"

make
make -C ../src libidle.so
trap 'echo -e "\n# \e[41mTest failed.\e[0m"' ERR

(
  rm .libidle_state
  LD_PRELOAD=${LD_PRELOAD:+${LD_PRELOAD}:}${IDLE_SO} ./accept &
  trap "kill $!" EXIT
  sleep 0.5
  timeout 5 flock -x .libidle_state echo "Locked."
  # accept
  test "$(cat .libidle_state)" == "1"
)

(
  rm .libidle_state
  LD_PRELOAD=${LD_PRELOAD:+${LD_PRELOAD}:}${IDLE_SO} ./receive &
  trap "kill $!" EXIT
  sleep 0.5
  timeout 5 flock -x .libidle_state echo "Locked."
  # accept (returns immediately), then recv
  test "$(cat .libidle_state)" == "2"
)

echo -e "\n# \e[30;42mTest successful.\e[0m"
