#define _GNU_SOURCE // needed for RTLD_NEXT

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define NON_NULL(S) do { if (!S) { fprintf(stderr, "couldn't load symbol: " #S "\n"); abort(); } } while (false)

static int (*next_accept)(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
static int (*next_pthread_create)(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine)(void*), void *arg);
static ssize_t (*next_recv)(int sockfd, void *buf, size_t len, int flags);
static int (*next_sem_destroy)(sem_t *sem);
static int (*next_sem_init)(sem_t *sem, int pshared, unsigned int value);
static int (*next_sem_post)(sem_t *sem);
static int (*next_sem_timedwait)(sem_t *sem, const struct timespec *abs_timeout);
static int (*next_sem_wait)(sem_t *sem);

/**
 * Records the number of pending wakeups on a semaphore
 * so we don't falsely believe we're idle when we're pending a wakeup.
 * When a thread goes to sleep on a semaphore, it is only counted
 * as idle if it has no pending wakeups.
 */
typedef struct {
    sem_t *sem;
    int pending_wakeups;
} SemaphoreInfo;

typedef struct {
    pthread_t id;
    bool sleeping;
    // non-null when waiting on a semaphore, requires sleeping=true
    sem_t *waiting_semaphore;
} ThreadInfo;

static struct {
    int filedes;
    bool locked;
    int times_idle;
    size_t sem_info_len;
    SemaphoreInfo *sem_info_ptr;
    size_t thr_info_len;
    ThreadInfo *thr_info_ptr;
} state = { 0 };

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

static bool threadinfo_is_blocked(ThreadInfo *thr_info)
{
    if (!thr_info->sleeping)
    {
        return false;
    }
    if (!thr_info->waiting_semaphore)
    {
        return true;
    }
    SemaphoreInfo *sem_info = libidle_find_sem_info(thr_info->waiting_semaphore);
    return sem_info->pending_wakeups > 0 ? false : true;
}

static pthread_mutex_t mutex;

// called when we've gone busy
static void libidle_lock()
{
    assert(!state.locked);
    flock(state.filedes, LOCK_EX);
    state.locked = true;
}

// called when we've gone idle
static void libidle_unlock()
{
    assert(state.locked);
    printf("unlock %i\n", state.filedes);
    lseek(state.filedes, 0, SEEK_SET);
    ftruncate(state.filedes, 0);
    dprintf(state.filedes, "%i\n", ++state.times_idle);
    flock(state.filedes, LOCK_UN);
    state.locked = false;
}

static void libidle_register_thread(pthread_t thread)
{
    state.thr_info_ptr = realloc(state.thr_info_ptr, sizeof(ThreadInfo) * ++state.thr_info_len);
    state.thr_info_ptr[state.thr_info_len - 1] = (ThreadInfo) {
        .id = thread,
        .sleeping = false,
        .waiting_semaphore = NULL,
    };
}

__attribute__ ((constructor))
void libidle_init()
{
    next_accept = dlsym(RTLD_NEXT, "accept");
    next_pthread_create = dlsym(RTLD_NEXT, "pthread_create");
    next_recv = dlsym(RTLD_NEXT, "recv");
    next_sem_destroy = dlsym(RTLD_NEXT, "sem_destroy");
    next_sem_init = dlsym(RTLD_NEXT, "sem_init");
    next_sem_post = dlsym(RTLD_NEXT, "sem_post");
    next_sem_timedwait = dlsym(RTLD_NEXT, "sem_timedwait");
    next_sem_wait = dlsym(RTLD_NEXT, "sem_wait");

    char *statefile = getenv("LIBIDLE_STATEFILE");
    if (!statefile) statefile = ".libidle_state";

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    state.filedes = open(statefile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    libidle_register_thread(pthread_self());
    libidle_lock();
}

static void libidle_entering_blocked_op()
{
    pthread_mutex_lock(&mutex);

    int is_active_threads = 0;
    pthread_t self = pthread_self();
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];
        if (thr_info->id == self)
        {
            thr_info->sleeping = true;
        }
        // printf("%lu: + blocked? %s\n", thr_info->id, threadinfo_is_blocked(thr_info) ? "yes" : "no");
        is_active_threads += threadinfo_is_blocked(thr_info) ? 0 : 1;
    }
    if (is_active_threads == 0)
    {
        libidle_unlock();
    }
    printf("%lu: + block -> %i%s\n", pthread_self(), is_active_threads, is_active_threads ? "" : " unlock");

    pthread_mutex_unlock(&mutex);
}

static void libidle_left_blocked_op()
{
    pthread_mutex_lock(&mutex);

    pthread_t self = pthread_self();
    for (int i = 0; i < state.thr_info_len; i++)
    {
        ThreadInfo *thr_info = &state.thr_info_ptr[i];

        // printf("%lu: - blocked? %s\n", thr_info->id, threadinfo_is_blocked(thr_info) ? "yes" : "no");
        if (thr_info->id == self)
        {
            thr_info->sleeping = false;
        }
    }
    printf("%lu: - blocked%s\n", pthread_self(), state.locked ? "" : "; lock");
    if (!state.locked)
    {
        libidle_lock();
    }
    pthread_mutex_unlock(&mutex);
}

//
// function proxies
//
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    NON_NULL(next_accept);
    libidle_entering_blocked_op();
    int ret = next_accept(sockfd, addr, addrlen);
    libidle_left_blocked_op();
    return ret;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
        void *(*start_routine) (void *), void *arg)
{
    NON_NULL(next_pthread_create);
    int ret = next_pthread_create(thread, attr, start_routine, arg);
    if (ret == 0)
    {
        libidle_register_thread(*thread);
    }
    return ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    NON_NULL(next_recv);
    libidle_entering_blocked_op();
    ssize_t ret = next_recv(sockfd, buf, len, flags);
    libidle_left_blocked_op();
    return ret;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    NON_NULL(next_sem_init);
    // register semaphore in SemaphoreInfo list (see above for rationale)
    int result = next_sem_init(sem, pshared, value);

    pthread_mutex_lock(&mutex);

    state.sem_info_ptr = realloc(state.sem_info_ptr, sizeof(SemaphoreInfo) * ++state.sem_info_len);
    state.sem_info_ptr[state.sem_info_len - 1] = (SemaphoreInfo) { .sem = sem, .pending_wakeups = value };

    pthread_mutex_unlock(&mutex);

    return result;
}

int sem_destroy(sem_t *sem)
{
    NON_NULL(next_sem_destroy);
    int result = next_sem_destroy(sem);

    pthread_mutex_lock(&mutex);

    for (int i = 0; i < state.sem_info_len; i++)
    {
        if (state.sem_info_ptr[i].sem == sem)
        {
            // swap with last entry
            state.sem_info_ptr[i] = state.sem_info_ptr[state.sem_info_len - 1];
            state.sem_info_ptr = realloc(state.sem_info_ptr, sizeof(SemaphoreInfo) * --state.sem_info_len);
            break;
        }
    }
    // otherwise, the semaphore is somehow not in our array
    // we should probably assert that, but eh.

    pthread_mutex_unlock(&mutex);

    return result;
}

int sem_post(sem_t *sem)
{
    NON_NULL(next_sem_post);

    int ret = next_sem_post(sem);

    pthread_mutex_lock(&mutex);
    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);
    if (sem_info)
    {
        sem_info->pending_wakeups++;
    }
    pthread_mutex_unlock(&mutex);
    return ret;
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout);

/**
    * When we signal a semaphore, we don't know which sleeping semaphore will wake up.
    * Because of this, we must track additionally the number of *pending* wakeups.
    * This is incremented in sem_post.
    * Conversely, sem_wait decrements.
    */
int sem_wait(sem_t *sem)
{
    return libidle_sem_wait(false, sem, NULL);
}

int sem_timedwait(sem_t *sem, const struct timespec *abs_timeout)
{
    return libidle_sem_wait(true, sem, abs_timeout);
}

static int libidle_sem_wait(bool timedwait, sem_t *sem, const struct timespec *abs_timeout)
{
    NON_NULL(next_sem_wait);

    pthread_mutex_lock(&mutex);
    SemaphoreInfo *sem_info = libidle_find_sem_info(sem);
    ThreadInfo *thr_info = libidle_find_thr_info(pthread_self());
    if (thr_info && sem_info)
    {
        thr_info->waiting_semaphore = sem_info->sem;
    }
    pthread_mutex_unlock(&mutex);

    libidle_entering_blocked_op();

    int ret;
    if (timedwait)
    {
        ret = next_sem_timedwait(sem, abs_timeout);
    }
    else
    {
        ret = next_sem_wait(sem);
    }

    /**
     * order matters here!
     * - unblock the op
     * - unlink the semaphore
     * - then decrement the wakeups.
     * the point is that we must enter a known state of wakefulness before we
     * untrack the semaphore. otherwise, libidle may miss the thread having woken.
     */
    pthread_mutex_lock(&mutex);
    libidle_left_blocked_op();
    if (thr_info)
    {
        thr_info->waiting_semaphore = NULL;
    }
    if (sem_info)
    {
        sem_info->pending_wakeups--;
    }
    pthread_mutex_unlock(&mutex);

    return ret;
}
