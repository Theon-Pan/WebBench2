#include "request.h"
#include "arguments.h"


/**
 * Responsible for HTTP and HTTP under TLS communication with server.
 * 
 * RETURNS:
 *      Positive number: Means successful communication, the number is the size of response in bytes;
 *                 Zero: Means successful communication with force mode;
 *      Negetive number: Means failed communication.
 */
int communicate(const Arguments *args, const HTTPRequest *http_request);



