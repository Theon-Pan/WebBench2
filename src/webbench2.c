#include "arguments.h"
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
}