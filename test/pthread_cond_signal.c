#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <unistd.h>

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  sem_t sem;
  int i;
} ThreadState;

/*
 * Test: wait for wakeup, send a notification
 */
void *sleep_on_cond(void *arg)
{
    ThreadState *state = (ThreadState*) arg;
    pthread_mutex_lock(&state->mutex);
    for (int i = 0; i < 100; i++)
    {
        while (state->i != i)
        {
            pthread_cond_wait(&state->cond, &state->mutex);
        }
        sem_post(&state->sem);
    }
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

int main()
{
    ThreadState state = { .i = -1 };
    pthread_mutex_init(&state.mutex, NULL);
    sem_init(&state.sem, 0, 0);
    pthread_cond_init(&state.cond, NULL);

    pthread_t thread;
    pthread_create(&thread, NULL, &sleep_on_cond, (void*) &state);

    for (int i = 0; i < 100; i++)
    {
        // because we lock here, the signal only happens
        // after pthread_cond_wait is already called
        pthread_mutex_lock(&state.mutex);
        state.i = i;
        pthread_cond_signal(&state.cond);
        pthread_mutex_unlock(&state.mutex);
        sem_wait(&state.sem);
    }
    void *ret;
    pthread_join(thread, &ret);
    sleep(2);
}
