#ifndef _COMMUNICATOR_H
#define _COMMUNICATOR_H

#include "request.h"
#include "arguments.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

/**
 * Send data through tls connection.
 * 
 * RETURNS:
 *      Positive number: The total bytes of the sent data.
 *      Negative number: Means failed communication.
 */
int send_tls_data(SSL *ssl, const char *data, int len);

/**
 * Receive data through TLS connection.
 * 
 * RETURNS:
 *      Positive number: The total bytes of the received data.
 *      Negative number: Means failed communication. 
 */
int recv_tls_data(SSL *ssl, char *buffer, int buffer_size);


/**
 * Responsible for HTTP and HTTP under TLS communication with server.
 * 
 * RETURNS:
 *      Positive number: Means successful communication, the number is the size of response in bytes;
 *                 Zero: Means successful communication with force mode;
 *      Negative number: Means failed communication.
 */
int communicate(const Arguments *args, const HTTPRequest *http_request);

#endif


