#ifndef _BENCH_POLL_H
#define _BENCH_POLL_H

#include "arguments.h"
#include "request.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

void bench_poll(const Arguments *args, const HTTPRequest *http_request);

#endif