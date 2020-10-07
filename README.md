# libidle
Determine when a process has "gone idle", ie. every thread is blocking.

## Rationale
When writing system tests, it's common to wait a second for the process being tested to come to rest and finish
processing some input. Preferably, you would instead wait for the request to complete or for some externally
queryable state to change, but this cannot always be done. The more generic test is simply to wait until the process
has "come to rest": meaning, all threads are stuck in blocking system calls that aren't going to wake up on their own:

- socket reads
- sleep calls
- waits on condition variables that haven't been signalled in another thread

When this state is reached, the test can safely proceed to the next stage.

## Usage
**Note: Since this library is under development, the following section is speculative.**

Preload the library when executing the process in question.
```
IDLE_STATE=tmpfile LD_PRELOAD=libidle.so process
```
The tmpfile will be created, if it doesn't exist.
While the process is busy, libidle will keep an exclusive lock on this file. Simply
try to open it for writing and you will block until the process is idle.

When the process is idle, the file will contain a serial number. Every time the process
goes busy, this number increments by one. This can be used to detect very short processes -
send your data to the process, then wait until it has gone idle and the serial number has changed.
