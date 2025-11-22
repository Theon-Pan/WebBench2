#ifndef _ARGUMENTS_H
#define _ARGUMENTS_H

#include <stdbool.h>

#define METHOD_GET 0
#define METHOD_HEAD 1
#define METHOD_OPTIONS 2
#define METHOD_TRACE 3
#define PROTOCOL_HTTP 0
#define PROTOCOL_HTTPS 1
#define HTTP_VERSION_0_9 0
#define HTTP_VERSION_1_0 1
#define HTTP_VERSION_1_1 2

#define DEFAULT_CLIENTS 1
#define DEFAULT_FORCE 0
#define DEFAULT_FORCE_RELOAD 0
#define DEFAULT_PROXY_PORT 80
#define DEFAULT_TARGET_PORT 80
#define DEFAULT_TARGET_SECURITY_PORT 443
#define DEFAULT_BENCH_TIME 30

#define HOSTNAMELEN 128
#define MAX_URL_LEN 128

typedef struct
{
    int clients;                   // How many http client to send request concurrently.
    int force;                     // 1 Ignore the response from server side; 0 Need waiting repsonse.
    int force_reload;              // Send the reload request.
    char proxy_host[HOSTNAMELEN];  // Host name of proxy.
    int proxy_port;                // Port number of proxy.
    char target_host[HOSTNAMELEN]; // Host name of testing target.
    int target_port;               // Port number of testing target.
    int bench_time;                // The duration of bench testing.
    int protocol;                  // HTTP or HTTPS.
    int http10;                    /* 0 - http/0.9; 1 - http/1.0; 2 - http/1.1 */
    int method;                    /* 0 - GET; 1 - HEAD; 2 - OPTIONS; 3 - TRACE */
    char url[MAX_URL_LEN];         /* Target URL.*/
} Arguments;

/**
 * Create a struct arguments with default values.
 */
Arguments create_default_arguments(void);

/**
 * Get arguments from command-line and set them to struct arguments.
 */
void set_arguments_values(int argc, char *argv[], Arguments *arguments);

/**
 * Show usage information.
 */
void usage(void);

/**
 * Validate the arguments parsed from command line.
 */
bool validate_arguments(const Arguments *args);

#endif
