#include <semaphore.h>

int main()
{
    sem_t semaphore;
    sem_init(&semaphore, 0, 0);
    sem_wait(&semaphore);
}
