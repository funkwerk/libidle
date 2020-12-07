# libidle
Determine when a process has "gone idle", ie. every thread is blocking.

## Rationale
When writing system tests, it's common to wait a second for the process being tested to come to rest and finish
processing some input. Preferably, you would instead wait for the request to complete or for some externally
queryable state to change, but this cannot always be done. The more generic test is simply to wait until the process
has "come to rest": meaning, all threads are stuck in blocking system calls that aren't going to wake up on their own:

- socket reads
- sleep calls
- waits on semaphores that haven't been posted in another thread
- waits on condition variables that haven't been signalled in another thread

When this state is reached, the test can safely proceed to the next stage.

## Usage

### Invocation
Preload the library when executing the process in question.
```
LIBIDLE_STATEFILE=tmpfile LD_PRELOAD=libidle.so process
```
The tmpfile will be created, if it doesn't exist.
While the process is busy, libidle will keep an exclusive lock on this file. Simply
try to open it for writing and you will block until the process is idle.

When the process is idle, the file will contain a serial number. Every time the process
goes busy, this number increments by one. This can be used to detect very short processes -
send your data to the process, then wait until it has gone idle and the serial number has changed.

### Verbose Output
Set `LIBIDLE_VERBOSE=` to see thread state changes printed to standard output.
On every state change, each thread's state will be printed in a row:

- '-' when busy (not in a known blocking call, ie. assumed to be running normally)
- 'b' when blocked, ie. waiting on a semaphore or condition variable
- 'B' when forced busy
- 's' when sleeping on a semaphore or condition variable
- 'S' when sleeping on a semaphore or condition variable that has already been signaled and is about to wake up
- 'i' when forced idle
- '?' when waiting on an unknown semaphore (this is a bug)

Lower-case letters indicate a thread that is considered "idle";
upper-case letters indicate a thread that is considered "busy".

Your program is considered idle if all threads are idle.

### Controlling libidle from your program
You can indicate to libidle that a thread should be considered busy, or considered idle, for a while.

```
void (*libidle_enable_forced_idle)() = dlsym(RTLD_DEFAULT, "libidle_enable_forced_idle");
void (*libidle_disable_forced_idle)() = dlsym(RTLD_DEFAULT, "libidle_disable_forced_idle");
void (*libidle_enable_forced_busy)() = dlsym(RTLD_DEFAULT, "libidle_enable_forced_busy");
void (*libidle_disable_forced_busy)() = dlsym(RTLD_DEFAULT, "libidle_disable_forced_busy");
```

For instance:

- While waiting for HTTP requests, set the serving thread to forced idle until a complete
request has been received.
- While requesting an external resource from a mock service, set the requesting thread to forced busy
to indicate you are expecting a response before the test may proceed.

## Details
### Implementing Condition Variables with Semaphores

Posix thread conditions are incredibly finicky. `pthread_cond_wait` and `pthread_cond_timedwait` can essentially
choose to wake up at any time for any reason. Because of this, they are unsuitable as basic primitives for a
"deterministic idleness" library.

Hence, libidle reimplements conditions on top of deterministic semaphores.

To facilitate this, every `pthread_cond_signal` behaves like `pthread_cond_broadcast`. This is valid because,
as said, `wait` and `timedwait` can choose to wake up at any time anyway.

`pthread_cond_wait` and `pthread_cond_broadcast` interact in a "condition frame" attached to each condition,
consisting of two semaphores, `in` and `out`. When a condition is signalled, it immediately creates a new condition
frame, ensuring that future waits will only be woken by future signals. It then considers the number
of waiting threads and posts 'n' semaphore tokens on `in`, then acquires 'n' tokens on `out`.

Each waiting thread similarly acquires a token on `in` (implementing the actual waiting), then posts a token on `out`.

After 'n' tokens have been acquired by the broadcasting thread, it knows that the condition frame is completed.
At this stage, it destroys and frees the associated semaphores.

Some care is required with timeouts. When a waiting thread times out, it must decrement the number of waiting threads.
However, it can occur that `signal` will signal a condition in the exact moment another thread is timing out.
When this happens, the signalling thread would see the wrong number of waiting threads, since the timed-out
thread had already returned. To address this, the waiting thread will check if the semaphore has already been
signaled via the `signaled` pointer, in which case it simply acquires a token on `in` and posts one on `out` - which it
now knows it can do without delay. Conversely, if the `signaled` flag is not set, the timed-out waiting thread knows
that its decrementing the number of waiting threads will be effective.
