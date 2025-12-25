#include "arguments.h"
#include "request.h"
#include "bench2.h"
#include "bench_select.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        usage();
        exit(EXIT_FAILURE);
    }

    Arguments args = create_default_arguments();
    set_arguments_values(argc, argv, &args);

    printf("bench time = %d, clients = %d, proxy_host = %s, proxy_port = %d, url = %s \n",
           args.bench_time, args.clients, args.proxy_host, args.proxy_port, args.url);

    HTTPRequest http_request = {0};
    build_request(&args, &http_request);

    // bench(&args, &http_request);
    // bench_with_no_racing(&args, &http_request);

    bench_select(&args, &http_request);
}