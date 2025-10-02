#ifndef _REQUEST_H
#define _REQUEST_H

#include "arguments.h"

#define MAXHOSTNAMELENGTH 128
#define REQUEST_BODY_SIZE 2048

typedef struct {
    char host[MAXHOSTNAMELENGTH];
    char body[REQUEST_BODY_SIZE];
} HTTPRequest;


/**
 * Build a http request string according the arguments parsed from command line.
 * RETURNS:
 *      Return negative if any error.
 */
int build_request(Arguments *args, HTTPRequest *request);

#endif