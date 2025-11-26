#ifndef _BENCH2_H
#define _BENCH2_H

#include "arguments.h"
#include "request.h"
#include <pthread.h>
#include <stdio.h>


typedef struct {
    const Arguments *args;
    const HTTPRequest *request;
    int thread_id;
    int *speed;
    int *failed;
    int *bytes;
    pthread_mutex_t *stats_mutex;
} BenchData;

typedef struct {
    const Arguments *args;
    const HTTPRequest *request;
    int thread_id;
    int speed;
    int failed;
    int bytes;
} BenchDataNoRace;

void bench(const Arguments *args, const HTTPRequest *http_request);

void bench_with_no_racing(const Arguments *args, const HTTPRequest *http_request);

#endif