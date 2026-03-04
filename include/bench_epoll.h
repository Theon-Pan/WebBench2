#ifndef _BENCH_EPOLL_H
#define _BENCH_EPOLL_H

#define MAX_PORT_NUMBER 65535
#define IS_VALID_PORT(port) ((port) > 1 && (port) <= MAX_PORT_NUMBER)

#include "arguments.h"
#include "request.h"


void bench_epoll(const Arguments *args, const HTTPRequest *http_request);

#endif
