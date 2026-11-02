/* SPDX-License-Identifier: GPL-3.0-only */
/* These programs must fail with the corresponding sanitizer diagnostic on the target. */
#include <pthread.h>
#include <stdlib.h>
#ifdef TEST_RACE
static pthread_barrier_t barrier;
static volatile int counter;
static void *race(void *p) {
    (void) p;
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 100000; i++) counter++;
    return NULL;
}
int main(void) {
    pthread_t a, b;
    if (pthread_barrier_init(&barrier, NULL, 2) || pthread_create(&a, NULL, race, NULL) ||
        pthread_create(&b, NULL, race, NULL)) return 2;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_barrier_destroy(&barrier);
    return 0;
}
#else
int main(void) {
    volatile unsigned index = 8;
    volatile char *p = malloc(4);
    if (!p) return 2;
    p[index] = 1;
    free((void *) p);
    return 0;
}
#endif
