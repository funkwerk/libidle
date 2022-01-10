#define _GNU_SOURCE // needed for RTLD_NEXT

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define NON_NULL(S) do { if (!S) { fprintf(stderr, "couldn't load symbol: " #S "\n"); abort(); } } while (false)

/**
 * Arrays consist of name_ptr and name_len.
 * Increase the length of this array by 1 and yield the new value.
 */
#define PUSH(array) array ## _ptr = realloc(array ## _ptr, sizeof(*array ## _ptr) * ++array ## _len);\
    array ## _ptr[array ## _len - 1]

/// Decrease the length of this array by 1.
#define DROP(array) array ## _ptr = realloc(array ## _ptr, sizeof(*array ## _ptr) * --array ## _len)

static int (*next_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static int (*next_pthread_cond_destroy)(pthread_cond_t *cond);
static int (*next_pthread_cond_init)(pthread_cond_t *restrict cond, const pthread_condattr_t *restrict attr);
static int (*next_pthread_create)(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine)(void*), void *arg);
static int (*next_pthread_join)(pthread_t thread, void **retval);
static int (*next_sem_destroy)(sem_t *sem);
static int (*next_sem_init)(sem_t *sem, int pshared, unsigned int value);
static sem_t *(*next_sem_open)(const char *name, int oflag, ...);
static int (*next_sem_post)(sem_t *sem);
static int (*next_sem_timedwait)(sem_t *sem, const struct timespec *abs_timeout);
static int (*next_sem_wait)(sem_t *sem);
static int (*next_pthread_setname_np)(pthread_t thread, const char *name);

/**
 * Records the number of pending wakeups on a semaphore
 * so we don't falsely believe we're idle when we're pending a wakeup.
 * When a thread goes to sleep on a semaphore, it is only counted
 * as idle if it has no pending wakeups.
 */
typedef struct {
    sem_t *sem; // semaphore is guaranteed/required to have a stable address.
    bool named_semaphore; // doesn't count as blocked because it gets external wakeups
    int pending_wakeups;
} SemaphoreInfo;

/**
 * Condition variables are basically impossible to support.
 *
 * This is because there is absolutely no determinism in them. When you say
 * `pthread_cond_signal`, "at least one" sleeping thread will wake up. Furthermore,
 * sleeping threads may wake up for any reason - a signal to another thread, an interrupt,
 * suspend/resume, an earthquake, who even knows. As such, we may end up in a situation
 * where we say that the process is idle, then a few ms later a `pthread_cond_wait`
 * spontaneously decides to return, messing up the process state.
 *
 * Because of this, we reimplement condition variables on top of semaphores, who are nice
 * and predictable, and for whom we already have handling anyways.
 *
 * This works like so: every condition variable initializes two semaphores, which we'll
 * call "IN" and "OUT". When a thread goes to sleep on a condition variable, it increments
 * the number of waiting threads, and `sem_wait`s on OUT.
 * When a thread tries to signal on the condition variable, it is always treated as a
 * broadcast. This is safe, because as said above, condition waiting threads may wake
 * up for basically any reason they want anyways.
 *
 * So when we `pthread_cond_broadcast`, we first copy out IN and OUT and reinitialize them
 * with fresh semaphores.
 * This is so that future waits will only be woken up by future signals.
 * Then we post waiting_threads tokens on OUT and await waiting_threads tokens on IN.
 * This is so that all threads that are currently using the OUT semaphore to sleep
 * finish touching it.
 * Finally, we can now free the two semaphores and return.
 *
 * Note that timeouts introduce a hole in this design, which is patched by "bool *signaled".
 */
typedef struct {
    pthread_cond_t *cond;
    // TODO one struct for all three (ConditionEventInfo?)
    // malloced because it needs to be stable across reallocs
    sem_t *in, *out;
    // set to true once the condition has been signaled, to allow us to collect and post late
    bool *signaled;
    int sleeping_threads;
    clockid_t clock;
} ConditionInfo;

/**
 * Because we want to support composition, the outermost override "counts".
 * Hence, instead of flags, we use a stack of `enum ForcedState`.
 */
enum ForcedState {
    /**
     * Forces the thread to remain marked as busy even if it is blocked.
     * This is so that things like HTTP clients can remain busy while
     * receiving a response. This is because waiting for a response to an
     * internally triggered request is not a source of idleness.
     * Has priority over forced_idle.
     */
    BUSY,
    /**
     * Forces the thread to remain marked as idle even if it is "awake".
     * This is so that things like message receive loops can avoid incrementing
     * the idle counter until they've received and dispatched a full message.
     *
     * Note that this should *never* be used to mark blocks that may wake
     * up in response to a signal.
     */
    IDLE,
};

typedef struct {
    pthread_t id;
    bool sleeping;

    size_t forced_state_len;
    enum ForcedState *forced_state_ptr;
    const char *name;

    // non-null when waiting on a semaphore, requires sleeping=true
    sem_t *waiting_semaphore;
} ThreadInfo;

static struct {
    bool initialized;

    // locks access to state
    pthread_mutex_t mutex;
    // since the mutex is recursive, we count the number of locks here
    // (this is used for restoring signal state on the last unlock)
    int mutex_locks;

    /*
     * To avoid a deadlock if a libidle function is interrupted by a signal while
     * holding the lock, we block all signals while the mutex is locked.
     * The original_mask field is used to restore the previous set of signals
     * after the lock has been released.
     * (Prompted by issues with parallel garbage collection in D 2.090. D uses signals
     * to freeze all but one thread. The frozen threads may be in libidle operations.)
     */
    sigset_t original_mask;

    int filedes;
    // file locked
    bool locked;
    int times_idle;
    bool verbose;

    size_t sem_info_len;
    SemaphoreInfo *sem_info_ptr;

    size_t cond_info_len;
    ConditionInfo *cond_info_ptr;

    size_t thr_info_len;
    ThreadInfo *thr_info_ptr;
} state = { 0 };

static ThreadInfo *find_thread_info();
static void vmaybe_lock(const char *fmt, va_list ap);
static void vmaybe_unlock(const char *fmt, va_list ap);
static void maybe_lock(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vmaybe_lock(fmt, args);
    va_end(args);
}
static void maybe_unlock(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vmaybe_unlock(fmt, args);
  va_end(args);
}
static void print_block_map();
static int num_active_threads();

static SemaphoreInfo *libidle_find_sem_info(sem_t *sem)
{
    for (int i = 0; i < state.sem_info_len; i++)
    {
        if (state.sem_info_ptr[i].sem == sem)
        {
            return &state.sem_info_ptr[i];
        }
    }
    return NULL;
}

static ConditionInfo *libidle_find_cond_info(pthread_cond_t *cond)
{
    for (int i = 0; i < state.cond_info_len; i++)
    {
        if (state.cond_info_ptr[i].cond == cond)
        {
            return &state.cond_info_ptr[i];
        }
    }
    return NULL;
}

static ThreadInfo *libidle_find_thr_info(pthread_t id)
{
    for (int i = 0; i < state.thr_info_len; i++)
    {
        if (state.thr_info_ptr[i].id == id)
        {
            return &state.thr_info_ptr[i];
        }
    }
    return NULL;
}

/**
 * Has this thread gone to sleep in a way that will prevent it from waking up on its own?
 */
static bool threadinfo_is_blocked(ThreadInfo *thr_info)
{
    if (thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[0] == BUSY)
    {
        return false;
    }
    if (thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[0] == IDLE)
    {
        return true;
    }
    if (!thr_info->sleeping)
    {
        return false;
    }
    if (!thr_info->waiting_semaphore)
    {
        return true;
    }
    SemaphoreInfo *sem_info = libidle_find_sem_info(thr_info->waiting_semaphore);
    return sem_info && sem_info->pending_wakeups > 0 ? false : true;
}

// called when we've gone busy
static void libidle_lock()
{
    assert(!state.locked);
    // printf("lock %i\n", state.filedes);
    flock(state.filedes, LOCK_EX);
    state.locked = true;
}

// called when we've gone idle
static void libidle_unlock()
{
    assert(state.locked);
    // printf("unlock %i\n", state.filedes);
    lseek(state.filedes, 0, SEEK_SET);
    ftruncate(state.filedes, 0);
    dprintf(state.filedes, "%i\n", ++state.times_idle);
    flock(state.filedes, LOCK_UN);
    state.locked = false;
}

static void libidle_lock_state_mutex()
{
    // block all signals while locked. prevents deadlocks if signal interrupts in in mid-operation.
    // state.mutex_locks is used to only store/reset the sigmask on the outermost lock/unlock
    // we call pthread_sigmask even if we're already locked (because we can't access state.mutex_locks
    // without the lock), but those calls have no issue since they don't corrupt the original_mask.
    sigset_t all_signals, original_mask;
    sigfillset(&all_signals);
    pthread_sigmask(SIG_SETMASK, &all_signals, &original_mask);
    pthread_mutex_lock(&state.mutex);
    state.mutex_locks ++;

    if (state.mutex_locks == 1)
    {
        // outermost lock
        state.original_mask = original_mask;
    }
}

static void libidle_unlock_state_mutex()
{
    bool was_outermost_lock = (state.mutex_locks == 1) ? true : false;
    sigset_t original_mask = state.original_mask;
    state.mutex_locks --;
    pthread_mutex_unlock(&state.mutex);
    if (was_outermost_lock)
    {
        pthread_sigmask(SIG_SETMASK, &original_mask, NULL);
    }
}

static void libidle_register_thread(pthread_t thread)
{
    libidle_lock_state_mutex();
    PUSH(state.thr_info) = (ThreadInfo) {
        .id = thread,
        .sleeping = false,
        .forced_state_ptr = NULL,
        .forced_state_len = 0,
        .waiting_semaphore = NULL,
    };
    libidle_unlock_state_mutex();
}

static void libidle_unregister_thread(pthread_t thread)
{
    libidle_lock_state_mutex();
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];
        if (thr_info->id == thread)
        {
            // swap with last entry and shrink
            *thr_info = state.thr_info_ptr[state.thr_info_len - 1];
            DROP(state.thr_info);
            break;
        }
    }
    libidle_unlock_state_mutex();
}

__attribute__ ((constructor))
void libidle_init()
{
    if (state.initialized) return;

    next_accept = dlsym(RTLD_NEXT, "accept");
    next_pthread_cond_destroy = dlvsym(RTLD_NEXT, "pthread_cond_destroy", "GLIBC_2.3.2");
    next_pthread_cond_init = dlvsym(RTLD_NEXT, "pthread_cond_init", "GLIBC_2.3.2");
    next_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    next_pthread_join = dlsym(RTLD_NEXT, "pthread_join");
    next_sem_destroy = dlvsym(RTLD_NEXT, "sem_destroy", "GLIBC_2.2.5");
    next_sem_init = dlvsym(RTLD_NEXT, "sem_init", "GLIBC_2.2.5");
    next_sem_open = dlsym(RTLD_NEXT, "sem_open");
    next_sem_post = dlvsym(RTLD_NEXT, "sem_post", "GLIBC_2.2.5");
    next_sem_timedwait = dlvsym(RTLD_NEXT, "sem_timedwait", "GLIBC_2.2.5");
    next_sem_wait = dlvsym(RTLD_NEXT, "sem_wait", "GLIBC_2.2.5");
    next_pthread_setname_np = dlvsym(RTLD_NEXT, "pthread_setname_np", "GLIBC_2.12");

    char *statefile = getenv("LIBIDLE_STATEFILE");
    if (!statefile) statefile = ".libidle_state";

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&state.mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    state.mutex_locks = 0;

    state.filedes = open(statefile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    state.verbose = getenv("LIBIDLE_VERBOSE") ? true : false;
    libidle_register_thread(pthread_self());
    libidle_lock();
    state.initialized = true;
}

static void entering_blocked_op(const char *fmt, ...)
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(!thr_info || thr_info->sleeping == false);
    if (thr_info) thr_info->sleeping = true;
    if (!thr_info || thr_info->forced_state_len == 0 || thr_info->forced_state_ptr[0] != BUSY) {
        va_list args;
        va_start(args, fmt);
        vmaybe_unlock(fmt, args);
        va_end(args);
    }

    libidle_unlock_state_mutex();
}

static void left_blocked_op(const char *fmt, ...)
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(!thr_info || thr_info->sleeping == true);
    if (thr_info) thr_info->sleeping = false;
    if (!thr_info || thr_info->forced_state_len == 0 || thr_info->forced_state_ptr[0] != BUSY) {
        va_list args;
        va_start(args, fmt);
        vmaybe_lock(fmt, args);
        va_end(args);
    }

    libidle_unlock_state_mutex();
}

// equivalent to entering a blocked op
void libidle_enable_forced_idle()
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);

    PUSH(thr_info->forced_state) = IDLE;

    maybe_unlock("libidle_enable_forced_idle()\n");

    libidle_unlock_state_mutex();
}

// equivalent to leaving a blocked op
void libidle_disable_forced_idle()
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);

    assert(thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[thr_info->forced_state_len - 1] == IDLE);
    DROP(thr_info->forced_state);

    maybe_lock("libidle_disable_forced_idle()\n");

    libidle_unlock_state_mutex();
}

void libidle_enable_forced_busy()
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);

    PUSH(thr_info->forced_state) = BUSY;

    /*printf("enable forced busy\n");
    void *buffer[16];
    backtrace(buffer, 16);
    backtrace_symbols_fd(buffer, 16, 1);*/

    libidle_unlock_state_mutex();
}

void libidle_disable_forced_busy()
{
    libidle_lock_state_mutex();

    ThreadInfo *thr_info = find_thread_info();
    assert(thr_info);

    assert(thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[thr_info->forced_state_len - 1] == BUSY);
    DROP(thr_info->forced_state);

    libidle_unlock_state_mutex();
}

static void vmaybe_lock(const char *fmt, va_list ap)
{
    /**
     * It is not the case that leaving a blocking op necessarily acquires lock.
     * For instance, the blocking op may be inside a forced_idle pair.
     * In that case, the lock is acquired when we disable forced_idle, bringing
     * the active thread count up.
     */
    if (state.verbose)
    {
        print_block_map();
        printf(": %lx: -block: ", pthread_self());
        vprintf(fmt, ap);
    }
    if (!state.locked && num_active_threads() > 0)
    {
        if (state.verbose)
        {
            printf("  lock\n");
        }
        libidle_lock();
    }
}

static void vmaybe_unlock(const char *fmt, va_list ap)
{
    if (state.verbose)
    {
        print_block_map();
        printf(": %lx: +block: ", pthread_self());
        vprintf(fmt, ap);
    }
    if (state.locked && num_active_threads() == 0)
    {
        if (state.verbose)
        {
            printf("  unlock\n");
        }
        libidle_unlock();
    }
}

static ThreadInfo *find_thread_info()
{
    pthread_t self = pthread_self();
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];

        if (thr_info->id == self)
        {
            return thr_info;
        }
    }
    return NULL;
}

static void print_block_map()
{
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];

        SemaphoreInfo *sem_info;
        if (thr_info->waiting_semaphore)
            sem_info = libidle_find_sem_info(thr_info->waiting_semaphore);
        if (i) printf("|");
        printf(
            (thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[0] == BUSY) ? "B" : // forced busy
            (thr_info->forced_state_len > 0 && thr_info->forced_state_ptr[0] == IDLE) ? "i" : // forced idle
            (!thr_info->sleeping) ? "-" : // computing
            (!thr_info->waiting_semaphore) ? "b" : // blocking busy
            !sem_info ? "?" : // sleeping on an unknown semaphore
            (sem_info->pending_wakeups > 0) ? "S" : // sleeping on a signaled semaphore
            "s"); // sleeping on a semaphore
        // printf(threadinfo_is_blocked(thr_info) ? "x" : "-");
    }
}

static int num_active_threads()
{
    int active_threads = 0;
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];
        active_threads += threadinfo_is_blocked(thr_info) ? 0 : 1;
    }
    return active_threads;
}

//
// function proxies
//
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    NON_NULL(next_accept);
    entering_blocked_op("accept()\n");
    int ret = next_accept(sockfd, addr, addrlen);
    left_blocked_op("accept()\n");
    return ret;
}

/**
 * pthread_create
 */

void remove_thread_info()
{
    libidle_unregister_thread(pthread_self());
}

struct ActualThreadInfo
{
    void *(*start_routine)(void*);
    void *arg;
    sem_t *started;
};

void *thread_wrapper(void *arg)
{
    NON_NULL(next_sem_post);
    libidle_register_thread(pthread_self());

    // now that we're registered, pthread_create can return

    struct ActualThreadInfo actual_thread_info = *(struct ActualThreadInfo*) arg;

    next_sem_post(actual_thread_info.started);

    void *ret; // pthread_cleanup_push and _pop are actually macros with unbalanced braces

    pthread_cleanup_push(&remove_thread_info, NULL);
    ret = actual_thread_info.start_routine(actual_thread_info.arg);
    pthread_cleanup_pop(true);
    return ret;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine) (void *), void *arg)
{
    NON_NULL(next_pthread_create);
    NON_NULL(next_sem_init);
    NON_NULL(next_sem_wait);
    NON_NULL(next_sem_destroy);

    struct ActualThreadInfo *actual_thread_info = malloc(sizeof(struct ActualThreadInfo));

    *actual_thread_info = (struct ActualThreadInfo) {
        .start_routine = start_routine,
        .arg = arg,
        .started = malloc(sizeof(sem_t)),
    };
    next_sem_init(actual_thread_info->started, 0, 0);

    // Despite blocking on a semaphore, we are busy: the other end of the semaphore is dangling
    // until next_pthread_create completes.
    libidle_enable_forced_busy();
    int ret = next_pthread_create(thread, attr, thread_wrapper, actual_thread_info);
    if (ret == 0)
    {
        // wait for the child thread to be started and registered so we don't prematurely unlock during startup
        next_sem_wait(actual_thread_info->started);
        next_sem_destroy(actual_thread_info->started);
        free(actual_thread_info->started);
        // child thread will have copied it
        free(actual_thread_info);
    }
    libidle_disable_forced_busy();
    return ret;
}

int pthread_join(pthread_t thread, void **retval)
{
    NON_NULL(next_pthread_join);
    entering_blocked_op("pthread_join()\n");
    // TODO wakeup signalling on thread destruction
    int ret = next_pthread_join(thread, retval);
    left_blocked_op("pthread_join()\n");
    return ret;
}

int pthread_setname_np(pthread_t thread, const char *name)
{
    NON_NULL(next_pthread_setname_np);
    int ret = next_pthread_setname_np(thread, name);
    if (state.verbose)
    {
      libidle_lock_state_mutex();
      for (int i = 0; i < state.thr_info_len; i++)
      {
          ThreadInfo *thr_info = &state.thr_info_ptr[i];
          if (thr_info->id == thread)
          {
              for (int k = 0; k < i; k++) printf("  ");
              printf("/ %s\n", name);
              thr_info->name = name;
              break;
          }
      }
      print_block_map();
      printf("\n");
      libidle_unlock_state_mutex();
    }
    return ret;
}

sem_t *sem_open(const char *name, int oflag, ...)
{
    // we may be called very early by libfaketime
    libidle_init();

    NON_NULL(next_sem_open);

    sem_t *ret;
    if (oflag & O_CREAT)
    {
        va_list args;
        va_start(args, oflag);
        mode_t mode = va_arg(args, mode_t);
        unsigned int value = va_arg(args, unsigned int);
        va_end(args);
        ret = next_sem_open(name, oflag, mode, value);
    }
    else
    {
        ret = next_sem_open(name, oflag);
    }
    if (ret == SEM_FAILED) return ret;

    libidle_lock_state_mutex();

    // register semaphore in SemaphoreInfo list
    PUSH(state.sem_info) = (SemaphoreInfo) { .sem = ret, .named_semaphore = true };

    libidle_unlock_state_mutex();

    return 0;
}

int sem_init_225(sem_t *sem, int pshared, unsigned int value)
{
    NON_NULL(next_sem_init);

    /**
     * see sem_init docs:
     *
     * > If  pshared  has  the  value  0,  then the semaphore is shared between the
     * > threads of a process, and should be located at some address that is visible
     * > to all threads (e.g., a global variable, or a variable allocated
     * > dynamically on the heap).
     */
    int ret = next_sem_init(sem, pshared, value);
    if (ret != 0) return ret;

    libidle_lock_state_mutex();

    // register semaphore in SemaphoreInfo list
    PUSH(state.sem_info) = (SemaphoreInfo) { .sem = sem, .pending_wakeups = value };

    libidle_unlock_state_mutex();

    return 0;
}

int sem_destroy_225(sem_t *sem)
{
    NON_NULL(next_sem_destroy);

    libidle_lock_state_mutex();

    // TODO iterate backwards- faster? more recently created sems are likely to be at the back
    for (int i = 0; i < state.sem_info_len; i++)
    {
        SemaphoreInfo *sem_info = &state.sem_info_ptr[i];
        if (sem_info->sem == sem)
        {
            // swap with last entry and shrink
            *sem_info = state.sem_info_ptr[state.sem_info_len - 1];
            DROP(state.sem_info);
            break;
        }
    }
    // should assert we actually freed something rn... meh

    libidle_unlock_state_mutex();

    return next_sem_destroy(sem);
}

int sem_post_225(sem_t *sem)
{
    NON_NULL(next_sem_post);

    libidle_lock_state_mutex();

    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);

    assert(sem_info);
    sem_info->pending_wakeups++;

    libidle_unlock_state_mutex();

    return next_sem_post(sem);
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout, const clockid_t clock);

/**
 * When we signal a semaphore, we don't know which sleeping semaphore will wake up.
 * Because of this, we must track additionally the number of *pending* wakeups.
 * The goal of this is to bridge the gap between the signalling thread going
 * idle and the sleeping thread waking up.
 * This is incremented in sem_post.
 * Conversely, sem_wait decrements.
 */
int sem_wait_225(sem_t *sem)
{
    return libidle_sem_wait(false, sem, NULL, 0);
}

int sem_timedwait_225(sem_t *sem, const struct timespec *abs_timeout)
{
    // "The timeout shall be based on the CLOCK_REALTIME clock."
    // -- POSIX spec, sem_timedwait
    return libidle_sem_wait(true, sem, abs_timeout, CLOCK_REALTIME);
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout, const clockid_t clock)
{
    NON_NULL(next_sem_wait);
    NON_NULL(next_sem_timedwait);

    libidle_lock_state_mutex();

    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);
    assert(sem_info);

    bool is_named_semaphore = sem_info->named_semaphore;

    ThreadInfo *thr_info = libidle_find_thr_info(pthread_self());
    assert(thr_info);

    if (thr_info->waiting_semaphore != NULL)
    {
        /**
         * There's already a semaphore wait going on.
         * This means we're being called from a signal handler, like the D GC handler.
         * Signal handlers should not participate in libidle operation, since they
         * are not part of the normal thread behavior.
         * So just ignore this one. sem_post may indicate pending wakeups, but we don't
         * need to consider them.
         */
        libidle_unlock_state_mutex();

        return next_sem_wait(sem);
    }

    if (!is_named_semaphore)
    {
        thr_info->waiting_semaphore = sem;
    }

    libidle_unlock_state_mutex();

    if (!is_named_semaphore)
    {
        entering_blocked_op("sem_wait()\n");
    }

    int ret;

    // EINTR == interrupted by a system call, just retry with the same parameters
    do
    {
        if (timedwait)
        {
            // we compensate for issues with faketime by
            // manually checking the clock.
            while (true)
            {
                ret = next_sem_timedwait(sem, abs_timeout);
                if (ret == -1 && errno == ETIMEDOUT)
                {
                    struct timespec ts;
                    clock_gettime(clock, &ts);
                    if (ts.tv_sec < abs_timeout->tv_sec
                        || (ts.tv_sec == abs_timeout->tv_sec
                            && ts.tv_nsec < abs_timeout->tv_nsec))
                    {
                        // timeout hasn't actually elapsed yet
                        continue;
                    }
                }
                break;
            }
        }
        else
        {
            ret = next_sem_wait(sem);
        }
    }
    while (ret == -1 && errno == EINTR);

    /**
     * order matters here!
     * - unblock the op
     * - unlink the semaphore
     * - then decrement the wakeups.
     * the point is that we must enter a known state of wakefulness before we
     * untrack the semaphore. otherwise, libidle may miss the thread having woken.
     */
    if (!is_named_semaphore)
    {
        libidle_lock_state_mutex();

        left_blocked_op("sem_wait()\n");

        // refind due to realloc
        thr_info = libidle_find_thr_info(pthread_self());
        sem_info = libidle_find_sem_info(sem);

        thr_info->waiting_semaphore = NULL;
        sem_info->pending_wakeups--;

        libidle_unlock_state_mutex();
    }

    return ret;
}

int pthread_cond_init_232(pthread_cond_t *restrict cond, const pthread_condattr_t *restrict attr)
{
    libidle_lock_state_mutex();
    // printf("  register %p\n", cond);

    clockid_t clock = CLOCK_REALTIME;
    if (attr)
    {
        pthread_condattr_getclock(attr, &clock);
    }

    // register condition in ConditionInfo list
    PUSH(state.cond_info) = (ConditionInfo) {
        .cond = cond,
        .in = malloc(sizeof(sem_t)),
        .out = malloc(sizeof(sem_t)),
        .signaled = malloc(sizeof(bool)),
        .sleeping_threads = 0,
        .clock = clock,
    };
    ConditionInfo *info = &state.cond_info_ptr[state.cond_info_len - 1];
    // our own function - not next_!
    sem_init_225(info->in, 0, 0);
    sem_init_225(info->out, 0, 0);
    *info->signaled = false;

    libidle_unlock_state_mutex();

    return 0;
}

int pthread_cond_destroy_232(pthread_cond_t *cond)
{
    NON_NULL(next_pthread_cond_destroy);

    libidle_lock_state_mutex();

    // TODO iterate backwards- faster? more recently created condvars are likely to be at the back
    for (int i = 0; i < state.cond_info_len; i++)
    {
        ConditionInfo *cond_info = &state.cond_info_ptr[i];
        if (cond_info->cond == cond)
        {
            // pthread_cond_destroy undefined if we're still waiting on this condition
            assert(cond_info->sleeping_threads == 0);

            sem_destroy_225(cond_info->in);
            sem_destroy_225(cond_info->out);
            free(cond_info->in);
            free(cond_info->out);
            free(cond_info->signaled);

            // swap with last entry and shrink
            *cond_info = state.cond_info_ptr[state.cond_info_len - 1];
            DROP(state.cond_info);
            break;
        }
    }
    // should assert we actually freed something rn... meh

    libidle_unlock_state_mutex();

    return next_pthread_cond_destroy(cond);
}

int pthread_cond_timedwait_232(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex,
    const struct timespec *restrict abstime)
{
    libidle_lock_state_mutex();

    // mutex is locked here per condition semantics. however, we can safely release it at this
    // point because we hold state.mutex anyway.
    pthread_mutex_unlock(mutex);

    ConditionInfo *cond_info = libidle_find_cond_info(cond);
    // assert(cond_info);

    // printf("> sleep on %p: sem %p, %i\n", cond, cond_info->in, !!abstime);

    cond_info->sleeping_threads++;
    sem_t *in = cond_info->in, *out = cond_info->out;
    bool *signaled = cond_info->signaled;
    clockid_t clock = cond_info->clock;

    libidle_unlock_state_mutex(); // all state modifications are done.

    int ret;
    if (abstime)
    {
        ret = libidle_sem_wait(true, in, abstime, clock);
        if (ret == -1 && errno == ETIMEDOUT)
        {
            // printf("! ! ! timeout case\n");
            libidle_lock_state_mutex();
            if (*signaled)
            {
                // printf("? ? ? already signaled\n");
                // consume our semaphore (will always succeed)
                // this situation happens if the condition was signaled after the timeout,
                // but before we got the lock - for instance, if it timed out while _broadcast held the lock.
                // In that case, _broadcast will not see our reduction in sleeping_threads,
                // so we must wait and post for it.
                sem_wait_225(in);
                sem_post_225(out);
            }
            else
            {
                // refind cause array may have realloced
                cond_info = libidle_find_cond_info(cond);
                cond_info->sleeping_threads--;
            }
            libidle_unlock_state_mutex();

            pthread_mutex_lock(mutex);

            // pthread_cond_timedwait reports errors differently from sem_timedwait
            return ETIMEDOUT;
        }
    }
    else
    {
        ret = sem_wait_225(in);
    }
    assert(ret == 0);
    // printf("cond waiter woke up.\n");
    sem_post_225(out);

    // grab the mutex back
    pthread_mutex_lock(mutex);

    return 0;
}

int pthread_cond_wait_232(pthread_cond_t *restrict cond, pthread_mutex_t *restrict mutex)
{
    return pthread_cond_timedwait_232(cond, mutex, NULL);
}

int pthread_cond_broadcast_232(pthread_cond_t *cond)
{
    libidle_lock_state_mutex();

    ConditionInfo *cond_info = libidle_find_cond_info(cond);
    ThreadInfo *thr_info = libidle_find_thr_info(pthread_self());

    assert(thr_info);
    assert(cond_info);

    sem_t *in = cond_info->in, *out = cond_info->out;
    bool *signaled = cond_info->signaled;
    int were_sleeping = cond_info->sleeping_threads;
    // printf("> broadcast to %i (%p)\n", were_sleeping, cond);

    // reinit cond_info - create a new "cond_wait/cond_signal group".
    cond_info->in = malloc(sizeof(sem_t));
    cond_info->out = malloc(sizeof(sem_t));
    cond_info->signaled = malloc(sizeof(bool));
    sem_init_225(cond_info->in, 0, 0);
    sem_init_225(cond_info->out, 0, 0);
    *cond_info->signaled = false;
    cond_info->sleeping_threads = 0;

    // printf("> distribute tokens\n");
    for (int i = 0; i < were_sleeping; i++)
    {
        // printf("post sem %p\n", in);
        sem_post_225(in);
    }
    *signaled = true;
    libidle_unlock_state_mutex(); // done with state mutation

    // printf("> collect tokens\n");
    for (int i = 0; i < were_sleeping; i++)
    {
        sem_wait_225(out);
    }
    // printf("> tokens collected\n");
    // because we've waited for out, we can now clean up the semaphores.
    sem_destroy_225(in);
    sem_destroy_225(out);
    free(in);
    free(out);
    free(signaled);

    return 0;
}

int pthread_cond_signal_232(pthread_cond_t *cond)
{
    // allowed under condition semantics!
    return pthread_cond_broadcast_232(cond);
}

// see http://blog.fesnel.com/blog/2009/08/25/preloading-with-multiple-symbol-versions/
// see https://code.woboq.org/userspace/glibc/nptl/pthread_cond_init.c.html
__asm__(".symver pthread_cond_broadcast_232, pthread_cond_broadcast@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_destroy_232, pthread_cond_destroy@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_init_232, pthread_cond_init@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_timedwait_232, pthread_cond_timedwait@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_wait_232, pthread_cond_wait@@GLIBC_2.3.2");
__asm__(".symver pthread_cond_signal_232, pthread_cond_signal@@GLIBC_2.3.2");
__asm__(".symver sem_destroy_225, sem_destroy@@GLIBC_2.2.5");
__asm__(".symver sem_init_225, sem_init@@GLIBC_2.2.5");
__asm__(".symver sem_post_225, sem_post@@GLIBC_2.2.5");
__asm__(".symver sem_wait_225, sem_wait@@GLIBC_2.2.5");
__asm__(".symver sem_timedwait_225, sem_timedwait@@GLIBC_2.2.5");
