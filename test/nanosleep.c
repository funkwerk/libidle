#include <time.h>

int main()
{
    struct timespec req = { .tv_sec = 60 };
    nanosleep(&req, NULL);
}
