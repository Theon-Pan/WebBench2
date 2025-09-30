#include <stdlib.h>
#include "request.h"


HTTPRequest build_request(Arguments *args) {
    HTTPRequest request = {0};

    /* Adjust the correct http procotol version according to the arguments from command line */
    
    // Only http 1.0 or above version support reloading, so need to reset the http protocol version.
    if (args->force_reload && strlen(args->proxy_host) > 0 && HTTP_VERSION_0_9 == args->http10) {
        args->http10 = HTTP_VERSION_1_0;
    }

    // HTTP method "HEAD" has been supported since http 1.0
    if (METHOD_HEAD == args->method && HTTP_VERSION_0_9 == args->http10) {
        args->http10 = HTTP_VERSION_1_0;
    }

    // HTTP method "OPTIONS" and "TRACE" have been supported since http 1.1
    if ((METHOD_OPTIONS == args->method) || (METHOD_TRACE == args->method) && args->http10 != HTTP_VERSION_1_1) {
        args->http10 = HTTP_VERSION_1_1;
    }


    // Construct the first line: add http method.
    switch (args->method) {
        case METHOD_GET:
            snprintf(request.body, sizeof(request.body), "GET ");
            break;
        case METHOD_HEAD:
            snprintf(request.body, sizeof(request.body), "HEAD ");
            break;
        case METHOD_OPTIONS:
            snprintf(request.body, sizeof(request.body), "OPTIONS ");
            break;
        case METHOD_TRACE:
            snprintf(request.body, sizeof(request.body), "TRACE ");
            break;
        default:
            return NULL;
    }




    return request;
}