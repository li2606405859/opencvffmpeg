//arn.cpp
#include <pthread.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void* thread_func(void* arg) {
    std::cout << "thread_func" << std::endl;
    int *val = (int*)arg;
    if(NULL != val) {
        printf("val = %d\n", *val);
    }
    return NULL;
}

int main() {
    pthread_t tid;
    int target = 100;
    if(pthread_create(&tid, NULL, thread_func, (void*)&target) != 0) {
        printf("pthread_create failed\n");
        return -1;
    }
   // sleep(1);
    printf("Main threa1111d \n");
    pthread_cancel(tid);
    return 0;
}


