#ifndef _REQUEST_H
#define _REQUEST_H

#include "arguments.h"

#define MAXHOSTNAMELENGTH 64
#define REQUEST_BODY_SIZE 2048

typedef struct {
    char host[MAXHOSTNAMELENGTH];
    char body[REQUEST_BODY_SIZE];
} HTTPRequest;


/**
 * Build a http request string according the arguments parsed from command line.
 * RETURNS:
 *      a struct HTTPRequest or NULL if there is any illegal arguments.
 */
HTTPRequest build_request(Arguments *args);

#endif