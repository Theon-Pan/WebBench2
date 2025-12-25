#include "bench2.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include "communicator.h"

double get_time_diff_ns(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

void* bench_worker(void *arg){
    BenchData *data = (BenchData *) arg;
    time_t start_time = time(NULL);
    int local_speed = 0;
    int local_failed = 0;
    int local_bytes = 0;

    printf("Thread [%d] started.\n", data->thread_id);

    while(time(NULL) - start_time < data->args->bench_time) {
        // Send http/https request to proxy or target server.
        int ret = communicate(data->args, data->request);

        if (ret >= 0)
        {
            local_bytes += ret;
            local_speed ++;
        }
        else if (ret < 0)
        {
            local_failed ++;
        }
        
        // Add a small delay to prevent infinite tight loop
        usleep(100000); // 100ms delay
        printf("Thread [%d] is working...\n", data->thread_id);
    }

    pthread_mutex_lock(data->stats_mutex);
    *(data->speed) += local_speed;
    *(data->failed) += local_failed;
    *(data->bytes) += local_bytes;
    pthread_mutex_unlock(data->stats_mutex);

    return NULL;

}

void* bench_worker_no_racing(void *arg){
    BenchDataNoRace *data = (BenchDataNoRace *) arg;
    time_t start_time = time(NULL);
    int local_speed = 0;
    int local_failed = 0;
    int local_bytes = 0;

    printf("Thread [%d] started.\n", data->thread_id);

    while(time(NULL) - start_time < data->args->bench_time) {
        // Send http/https request to proxy or target server.
        int ret = communicate(data->args, data->request);
        if (ret > 0)
        {
            local_bytes += ret;
            local_speed ++;
        }
        else
        {
            local_failed ++;
        }

        // Add a small delay to prevent infinite tight loop
        usleep(100000); // 100ms delay
        printf("Thread [%d] working...\n", data->thread_id);
    }

   
    (data->speed) += local_speed;
    (data->failed) += local_failed;
    (data->bytes) += local_bytes;
    

    return NULL;

}


void bench(const Arguments *args, const HTTPRequest *http_request) {
    struct timespec request_start, request_end;

    if (NULL == args || NULL == http_request) {
        fprintf(stderr, "No args or request to bench.\n");
        return;
    }

    // Initilize threading variables.
    pthread_t *threads = malloc(args->clients * sizeof(pthread_t));
    BenchData *bench_data = malloc(args->clients * sizeof(BenchData));
    if (!threads || !bench_data) {
        fprintf(stderr, "Memory allocation for threads failed\n");
        return;
    }
    pthread_mutex_t stats_mutext = PTHREAD_MUTEX_INITIALIZER;

    int total_speed = 0;
    int total_failed = 0;
    int total_bytes = 0;

    clock_gettime(CLOCK_MONOTONIC, &request_start);

    printf("Starting %d threads for %d seconds...\n", args->clients, args->bench_time);

    // Create threads
    for (int i = 0; i < args->clients; i++) {
        bench_data[i].args = args;
        bench_data[i].request = http_request;
        bench_data[i].thread_id = i;
        bench_data[i].speed = &total_speed;
        bench_data[i].failed = &total_failed;
        bench_data[i].bytes = &total_bytes;
        bench_data[i].stats_mutex = &stats_mutext;        

        if (pthread_create(&threads[i], NULL, bench_worker, &bench_data[i])) {
            fprintf(stderr, "Failed to create thread [%d]\n", i);
            return;
        }

    }

    // Wait for all threads to complete
    for (int i = 0; i < args->clients; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &request_end);
    // Print final statistics
    printf("\n=== Benchmark Resuls ===\n");
    printf("Total speed: %d\n", total_speed);
    printf("Total failed: %d\n", total_failed);
    printf("Total bytes: %d\n", total_bytes);

    double request_time = get_time_diff_ns(request_start, request_end);
    printf("The time spent with racing: %.9f seconds.\n", request_time);

    // Cleanup
    free(threads);
    free(bench_data);
    pthread_mutex_destroy(&stats_mutext);

    return;
}

void bench_with_no_racing(const Arguments *args, const HTTPRequest *http_request)
{
    struct timespec request_start, request_end;

    if (NULL == args || NULL == http_request) {
        fprintf(stderr, "No args or request to bench.");
        return;
    }

    // Initilize threading variables.
    pthread_t *threads = malloc(args->clients * sizeof(pthread_t));

    BenchDataNoRace *bench_data_no_race = malloc(args->clients * sizeof(BenchDataNoRace));
    if (!threads || !bench_data_no_race) {
        fprintf(stderr, "Memory allocation for threads failed\n");
        return;
    }

    int total_speed = 0;
    int total_failed = 0;
    int total_bytes = 0;

    clock_gettime(CLOCK_MONOTONIC, &request_start);

    printf("Starting %d threads for %d seconds...\n", args->clients, args->bench_time);

    // Create threads
    for (int i = 0; i < args->clients; i++) {
        bench_data_no_race[i].args = args;
        bench_data_no_race[i].request = http_request;
        bench_data_no_race[i].thread_id = i;
        bench_data_no_race[i].speed = 0;
        bench_data_no_race[i].failed = 0;
        bench_data_no_race[i].bytes = 0;

        if (pthread_create(&threads[i], NULL, bench_worker_no_racing, &bench_data_no_race[i])) {
            fprintf(stderr, "Failed to create thread [%d]\n", i);
            return;
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < args->clients; i++) {
        pthread_join(threads[i], NULL);
    }

    
    // Summary
    for (int i = 0; i < args->clients; i++) {
        total_bytes += bench_data_no_race[i].bytes;
        total_failed += bench_data_no_race[i].failed;
        total_speed += bench_data_no_race[i].speed;
    }

    clock_gettime(CLOCK_MONOTONIC, &request_end);
    // Print final statistics
    printf("\n=== Benchmark Resuls ===\n");
    printf("Total speed: %d\n", total_speed);
    printf("Total failed: %d\n", total_failed);
    printf("Total bytes: %d\n", total_bytes);

    double request_time = get_time_diff_ns(request_start, request_end);
    printf("The time spent bench with no race: %.9f seconds.\n", request_time);

    // Cleanup
    free(threads);
    free(bench_data_no_race);
}

