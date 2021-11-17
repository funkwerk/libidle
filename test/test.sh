#!/usr/bin/env bash
set -euxo pipefail

IDLE_SO="../src/libidle.so"

make
make -C ../src libidle.so
trap 'echo -e "\n# \e[41mTest failed.\e[0m"' ERR

function expect_locked() {
  CMD="$1"
  EXPECTED_STATE="$2"
  rm .libidle_state || true
  LD_PRELOAD=${LD_PRELOAD:+${LD_PRELOAD}:}${IDLE_SO} eval "$CMD &"
  PROC=$!
  trap "kill $PROC" RETURN
  sleep 0.5
  flock --timeout 5 -x .libidle_state echo "Locked."
  # accept
  test "$(cat .libidle_state)" == "$EXPECTED_STATE"
}

function expect_not_locked() {
  CMD="$1"
  rm .libidle_state || true
  LD_PRELOAD=${LD_PRELOAD:+${LD_PRELOAD}:}${IDLE_SO} eval "$CMD &"
  PROC=$!
  trap "kill $PROC" RETURN
  sleep 0.5 # ensure the process has a lock on the file
  # shouldn't get a lock within 1s
  ! flock --timeout 1 -x .libidle_state echo "Locked."
}

# one call: accept
expect_locked 'build/accept' '1'
expect_locked 'build/sem_wait' '1'
# this cluster of tests bounces a signal between two threads. the check is that we should not
# go idle at any point during it.
expect_not_locked 'build/sem_post'
expect_not_locked 'build/pthread_cond_signal'

echo -e "\n# \e[30;42mTest successful.\e[0m"
