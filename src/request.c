#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "request.h"


int build_request(Arguments *args, HTTPRequest *request) {

    // Clean the request from caller.
    memset(request, 0, sizeof(*request));
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
    if ((METHOD_OPTIONS == args->method || METHOD_TRACE == args->method) && (args->http10 != HTTP_VERSION_1_1)) {
        args->http10 = HTTP_VERSION_1_1;
    }


    // Construct the first line of the HTTP request body: add http method.
    switch (args->method) {
        case METHOD_GET:
            snprintf(request->body, sizeof(request->body), "GET ");
            break;
        case METHOD_HEAD:
            snprintf(request->body, sizeof(request->body), "HEAD ");
            break;
        case METHOD_OPTIONS:
            snprintf(request->body, sizeof(request->body), "OPTIONS ");
            break;
        case METHOD_TRACE:
            snprintf(request->body, sizeof(request->body), "TRACE ");
            break;
        default:
            return -1;
    }

    // Construct the first line of the HTTP request body: append the hostname.
    if (0 == strlen(args->proxy_host)) {
        //If no proxy specified, then the hostname in the first line is '/'.
        strncat(request->body, "/", sizeof(request->body) - strlen(request->body) - 1);
    }else{
        //If proxy is specified, the use url as hostname in the first line.
        strncat(request->body, args->url, sizeof(request->body) - strlen(request->body) - 1);
    }
    
    
    // Construct the first line of the HTTP request body: append the http protocol version.
    if (HTTP_VERSION_1_0 == args->http10) {
        strncat(request->body, " HTTP/1.0\r\n", sizeof(request->body) - strlen(request->body) - 1);
    }else if (HTTP_VERSION_1_1 == args->http10) {
        strncat(request->body, " HTTP/1.1\r\n", sizeof(request->body) - strlen(request->body) - 1);
    }
    
    // End of the first line.
    strncat(request->body, "\r\n", sizeof(request->body) - strlen(request->body) - 1);

    snprintf(request->host, sizeof(request->host), "%s", args->target_host);

    return 0;
}