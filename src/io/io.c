#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

struct timeval time_start, time_end;
int end_set;
int flags[2];

int input(void)
{
    int a;
    scanf("%d", &a);
    gettimeofday(&time_start, NULL);
    end_set = 0;
    return a;
}

void output(int a)
{
    if (end_set == 0)
    {
        gettimeofday(&time_end, NULL);
        end_set = 1;
    }
    printf("%d\n", a);
}

void outputFloat(float a)
{
    if (end_set == 0)
    {
        gettimeofday(&time_end, NULL);
        end_set = 1;
    }
    printf("%f\n", a);
}

void add_lab4_flag(int idx, int val)
{
    flags[idx] += val;
}

__attribute((constructor)) void before_main(void)
{
    flags[0] = 0;
    flags[1] = 0;
    end_set = 0;
    gettimeofday(&time_start, NULL);
}

__attribute((destructor)) void after_main(void)
{
    long time_us = 0;
    fprintf(stderr, "Allocate Size (bytes):\n%d\n", flags[0]);
    fprintf(stderr, "Execute Cost:\n%d\n", flags[1]);
    if (end_set == 0)
    {
        gettimeofday(&time_end, NULL);
    }
    time_us += 1000000L * (time_end.tv_sec - time_start.tv_sec) +
        time_end.tv_usec - time_start.tv_usec;
    fprintf(stderr, "Take Times (us):\n%ld\n", time_us);
}
