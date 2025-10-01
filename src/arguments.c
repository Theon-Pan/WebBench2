#include "arguments.h"
#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>

bool is_https(const char *url);

bool is_supported_url(const char *url);

int set_arguments_from_url(Arguments *args);

Arguments create_default_arguments(void) {
    Arguments arg = {0};
    arg.bench_time = DEFAULT_BENCH_TIME;
    arg.clients = DEFAULT_CLIENTS;
    arg.force = DEFAULT_FORCE;
    arg.force_reload = DEFAULT_FORCE_RELOAD;
    arg.method = METHOD_GET;
    arg.protocol = PROTOCOL_HTTP;
    arg.proxy_port = DEFAULT_PROXY_PORT;
    arg.target_port = DEFAULT_TARGET_PORT;
    return arg;
}

bool validate_arguments(const Arguments *args) {
    bool is_arguments_valid = false;

    /**
     * So far, we only support the http method for benching as below list:
     *      "GET"
     *      "HEAD"
     *      "OPTIONS"
     *      "TRACE"
     */
    switch(args->method) {
        case METHOD_GET:
        case METHOD_HEAD:
        case METHOD_OPTIONS:
        case METHOD_TRACE:
            is_arguments_valid = true;
            break;
        default:
            break;
    }

    return is_arguments_valid;
}

void set_arguments_values(int argc, char *argv[], Arguments *args) {
    int opt;
    int options_index = 0;
    char *short_opts = "921Vfrt:p:c:?h";
    char *endptr;
    char *tmp = NULL;
    long t;
    
    if (NULL == args) {
        fprintf(stderr, "No Options structure defined to carry on the command-line args.");
        exit(EXIT_FAILURE);
    }

    const struct option long_options[] = {
        {"force", no_argument, &(args->force), 1},
        {"reload", no_argument, &(args->force_reload), 1},
        {"time", required_argument, NULL, 't'},
        {"help", no_argument, NULL, '?'},
        {"http09", no_argument, NULL, '9'},
        {"http10", no_argument, NULL, '1'},
        {"http11", no_argument, NULL, '2'},
        {"get", no_argument, &(args->method), METHOD_GET},
        {"head", no_argument, &(args->method), METHOD_HEAD},
        {"options", no_argument, &(args->method), METHOD_OPTIONS},
        {"trace", no_argument, &(args->method), METHOD_TRACE},
        {"version", no_argument, NULL, 'V'},
        {"proxy", required_argument, NULL, 'p'},
        {"clients", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    // Get the command line options
    while ((opt = getopt_long(argc, argv, short_opts, long_options, &options_index)) != EOF) {
        switch (opt) {
            case 0: break;
            case 'f': args->force = 1; break;
            case 'r': args->force_reload = 1; break;
            case '9': args->http10 = 0; break;
            case '1': args->http10 = 1; break;
            case '2': args->http10 = 2; break;
            case 'V': printf("2.0\n"); exit(EXIT_SUCCESS);          //TODO: need to be replaced by a constant.
            case 't': 
                errno = 0;
                t = strtol(optarg, &endptr, 10);
                if(errno != 0 || endptr == optarg || t <= 0) {
                    fprintf(stderr, "Invalid option --time %s: Illegal format.\n", optarg);
                    exit(EXIT_FAILURE);
                }
                args->bench_time = (int) t; 
                break;
            case 'p':
                /* Parse proxy server string, with the format "hostname:port". */
                snprintf(args->proxy_host, sizeof(args->proxy_host), "%s", optarg);
                tmp = strrchr(args->proxy_host, ':');
                /**
                 * If the proxy server string does not contain ':', then treat the whole string as the host name,
                 * 80 as the default port number.
                 */
                if (tmp == NULL) {
                    args->proxy_port = 80;
                    break;
                }

                /* If the proxy server string likes "localhost:", then using 80 as the default port number*/
                if (tmp == args->proxy_host + strlen(args->proxy_host) - 1) {
                    args->proxy_port = 80;
                }
                /* The proxy server string should only be the format "hostname:port". */
                else {
                    errno = 0;
                    t = strtol(tmp + 1, &endptr, 10);
                    if (errno != 0 || (tmp + 1) == endptr || t <=0) {
                        fprintf(stderr, "Invalid option --proxy %s: Illegal port number '%s'.\n", optarg, tmp + 1);
                        exit(EXIT_FAILURE);
                    }
                    args->proxy_port = (int) t;
                    
                    /* If the proxy server string likes ":7899", then use "127.0.0.1" as the host name.*/
                    if (tmp == args->proxy_host) {
                        snprintf(args->proxy_host, sizeof(args->proxy_host), "%s", "127.0.0.1");
                        break;
                    }
                }
                if (tmp != NULL) {
                    *tmp = '\0';
                }
                break;
            case ':':
            case 'h':
            case '?':
                usage();
                exit(EXIT_FAILURE);
            case 'c':
                errno = 0;
                t = strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || t <= 0) {
                    fprintf(stderr, "Invalid option --clients %s: Illegal number.\n", optarg);
                    exit(EXIT_FAILURE);
                }
                args->clients = (int) t;
                break;
        }
    }
    /* No positional arguments in command line, means no URL speicified. */
    if (optind == argc) {
        fprintf(stderr, "webbench2: Missing URL!\n");
        usage();
        exit(EXIT_FAILURE);
    }

    /* Check if the url is a valid one. */
    if (strlen(argv[optind]) > 1500) {
        fprintf(stderr, "Invalid url %s: URL is too long.\n", argv[optind]);
        exit(EXIT_FAILURE);
    }
    if (!is_supported_url(argv[optind])) {
        fprintf(stderr, "Invalid url %s: Only HTTP/HTTPS protocol is supported, set --proxy for others.\n", argv[optind]);
        exit(EXIT_FAILURE);
    } 

    /* Set the target url specified in command line. */
    snprintf(args->url, sizeof(args->url), "%s", argv[optind]);
    // If url does not end with '/', then append.
    if ('/' != argv[optind][strlen(argv[optind]) - 1]) {
        strncat(args->url, "/", sizeof(args->url) - strlen(args->url) - 1);
    }

    if (set_arguments_from_url(args) < 0) {
        exit(EXIT_FAILURE);
    }
    
    if (false == validate_arguments(args)) {
        fprintf(stderr, "Illegal command line arguments!\n");
        usage();
        exit(EXIT_FAILURE);
    }

    
}

void usage(void) {
    fprintf(stderr, 
        "webbench [option]... URL\n"
        "  -f|--force               Don't wait for reply from server.\n"
        "  -r|--reload              Send reload request - Pragma: no-cache.\n"
        "  -t|--time <sec>          Run benchmark for <sec> seconds. Default 30.\n"
        "  -p|--proxy <server:port> Use proxy server for request.\n"
        "  -c|--clients <n>         Run <n> HTTP clients at once. Default one.\n"
        "  -9|--http09              Use HTTP/0.9 style requests.\n"
        "  -1|--http10              Use HTTP/1.0 protocol.\n"
        "  -2|--http11              Use HTTP/1.1 protocol.\n"
        "  --get                    Use GET request method.\n"
        "  --head                   Use HEAD request method.\n"
        "  --options                Use OPTIONS request method.\n"
        "  --trace                  Use TRACE request method.\n"
        "  -?|-h|--help             This information.\n"
        "  -V|--version             Display program version.\n"
    );
}

bool is_https(const char *url) {
    return strncasecmp("https://", url, 8) == 0;
}

bool is_supported_url(const char *url) {
    /* The url should contain "http://" or "https://" prefix */
    return (strncasecmp("http://", url, 7) == 0 || strncasecmp("https://", url, 8) == 0);
}

int set_arguments_from_url(Arguments *args) {
    int hostname_start_offset = 0;
    char port_str[10] = {0};
    char *first_comma_index_in_url, *first_splash_index_in_url;
    char *endptr = NULL;
    long t;

    if (0 == strlen(args->url)) {
        fprintf(stderr, "Invalid url: url is empty or null.\n");
        return -1;
    }
    
    if ('/' != args->url[strlen(args->url) - 1]) {
        fprintf(stderr, "Invalid url %s: url does not end with '/'.\n", args->url);
        return -1;
    }

    // Check the url's protocol part.
    if (is_https(args->url)) {
        args->protocol = PROTOCOL_HTTPS;
        args->target_port = DEFAULT_TARGET_SECURITY_PORT;
    }

    hostname_start_offset = strstr(args->url,"://") - args->url + 3;
    
    /* Get target host and port from url. */
    first_comma_index_in_url = strchr(args->url + hostname_start_offset, ':');
    first_splash_index_in_url = strchr(args->url + hostname_start_offset, '/');
    if (NULL != first_comma_index_in_url && NULL != first_splash_index_in_url 
        && (first_comma_index_in_url < first_splash_index_in_url)) {
        // It means there's target port number specified, we need to parse and get it.
        // Parse and get hostname first.
        strncpy(args->target_host, 
            args->url + hostname_start_offset, 
            first_comma_index_in_url - args->url - hostname_start_offset);
        
        // Then parse and get target port number
        strncpy(port_str, 
            first_comma_index_in_url + 1, 
            first_splash_index_in_url - first_comma_index_in_url - 1);
        
        t = strtol(port_str, &endptr, 10);
        if (errno != 0 || port_str == endptr || t <= 0) {
            fprintf(stderr, "Invalid port number in url %s.\n", args->url);
            return -1;
        }
        args->target_port = (int) t;
    }else if (NULL == first_comma_index_in_url) {
        // It means there's no target port number specified, just parse the host.
        strncpy(args->target_host,
            args->url + hostname_start_offset, 
            first_splash_index_in_url - args->url - hostname_start_offset);

    }
    return 0;
}
