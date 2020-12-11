# TODO
- ensure that `pthread_cond_notify` marks itself as forced busy to bypass the issue of delivering a signal
  to a forced idle thread causing both threads to temporarily be considered idle.
- since we'll often be used in conjunction with libfaketime, compensate for its issues with timeouts by looping
  in threading functions until the timeout is *actually* expired.
