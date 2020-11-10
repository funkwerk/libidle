#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

/*
 * Test: bounce the ball between both threads 100 times.
 * During this, libidle should never unlock.
 */
void *sleep_on_sem(void* state)
{
    sem_t *semaphore = (sem_t*) state;
    for (int i = 0; i < 100; i++)
    {
        sem_wait(&semaphore[0]);
        sem_post(&semaphore[1]);
    }
    return NULL;
}

int main()
{
    sem_t semaphore[2];
    sem_init(&semaphore[0], 0, 0);
    sem_init(&semaphore[1], 0, 0);

    pthread_t thread;
    pthread_create(&thread, NULL, &sleep_on_sem, (void*) semaphore);

    for (int i = 0; i < 100; i++)
    {
        sem_post(&semaphore[0]);
        sem_wait(&semaphore[1]);
    }
    void *ret;
    pthread_join(thread, &ret);
    sleep(2);
}
