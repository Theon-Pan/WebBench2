#include "communicator.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <pthread.h>

static pthread_once_t ssl_init_once = PTHREAD_ONCE_INIT;

static void init_ssl_lib()
{
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    printf("SSL library initialized\n");
}

// Call this before any SSL operations.
static void ensure_ssl_initialized()
{
    // Ensure the init_ssl_lib routine be run only once.
    pthread_once(&ssl_init_once, init_ssl_lib);
}

// Create SSL context.
static SSL_CTX* create_ssl_context()
{
    SSL_CTX *ctx;
    
    ctx = SSL_CTX_new(TLS_client_method());
    if (NULL == ctx) {
        perror("Unable to create a new SSL context.");
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    // @todo: Need to change to SSL_VERIFY_PEER 
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    return ctx;
}

// Send data over TLS connection.
int send_tls_data(SSL *ssl, const char *data, int len)
{
    int bytes_sent = 0;
    int total_sent = 0;
    int error;

    if (ssl == NULL || data == NULL || len <= 0)
    {
        return -1;
    }

    while (total_sent < len)
    {
        bytes_sent = SSL_write(ssl, data + total_sent, len - total_sent);
        if (bytes_sent <= 0)
        {
            error = SSL_get_error(ssl, bytes_sent);
            switch (error)
            {
                case SSL_ERROR_WANT_WRITE:
                case SSL_ERROR_WANT_READ:
                    continue;
                case SSL_ERROR_SYSCALL:
                    if (bytes_sent == 0)
                    {
                        fprintf(stderr, "SSL_write: Unexpected EOF\n");
                    }
                    else
                    {
                        perror("SSL_write syscall error");
                        ERR_print_errors_fp(stderr);
                    }
                    return -1;
                case SSL_ERROR_ZERO_RETURN:
                    return -1;
                case SSL_ERROR_SSL:
                    ERR_print_errors_fp(stderr);
                    return -1;
                default:
                    fprintf(stderr, "SSL write error: %d\n", error);
                    ERR_print_errors_fp(stderr);
                    return -1;
            }

            if (error == SSL_ERROR_WANT_WRITE)
            {
                continue; // Send buffer has been filled, need retry.
            }
            fprintf(stderr, "SSL write error: %d\n", error);
            return -1;
        }
        total_sent += bytes_sent;
    }

    return total_sent;
}

// Receive data over TLS connection.
int recv_tls_data(SSL *ssl, char *buffer, int buffer_size)
{
    int bytes_received = 0;
    int total_received = 0;
    int error;

    while (total_received < buffer_size - 1)
    {
        bytes_received = SSL_read(ssl, buffer + total_received, buffer_size - total_received - 1);
        if (bytes_received <= 0)
        {
            error = SSL_get_error(ssl, bytes_received);
            if (error == SSL_ERROR_WANT_READ)
            {
                continue; // Received buffer is empty, no more data to be read, need retry.
            }
            fprintf(stderr, "SSL read error: %d\n", error);
            return -1;
        }
        total_received += bytes_received;
        
        buffer[total_received] = '\0';
        // Only receive http response headers. So here we just check if we get the complete HTTP response headers.
        if (strstr(buffer, "\r\n\r\n"))
        {
            break;
        }
    }

    return total_received;
}


static int Socket(const char *host, const int port)
{
    if (NULL == host || port <= 0)
    {
        fprintf(stderr, "No host:port specified.\n");
        return -1;
    }
    int sockfd;
    struct addrinfo hints = {
        .ai_family = AF_INET,       // IPv4
        .ai_socktype = SOCK_STREAM, // TCP
        .ai_flags = 0,
        .ai_protocol = 0};
    struct addrinfo *result, *rp;
    char port_str[16] = {0};

    // Convert port number to string.
    snprintf(port_str, sizeof(port_str), "%d", port);

    // Get address information.
    int status = getaddrinfo(host, port_str, &hints, &result);
    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }

    // Try each address until we connect successfully.
    rp = result;
    while (rp != NULL)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1)
        {
            continue; // Try next address.
        }
        
        // Connect to check if the sockfd is available. If not, close the sockfd and try the next.
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == -1)
        {
            close(sockfd);
            rp = rp->ai_next;
            continue; // Try next address.
        }
        else
        {
            break;
        }

    }

    freeaddrinfo(result);

    // Means no address is available to use, report error and return -1
    if (rp == NULL)
    {
        fprintf(stderr, "Can not connect to %s:%d\n", host, port);
        return -1;
    }

    return sockfd;
}

static int handle_proxy_connect(const char *proxy_host, const int proxy_port, const char *target_host, const int target_port)
{
    char connect_request[1024] = {0};
    char connect_response[1024] = {0};

    if (NULL == proxy_host || NULL == proxy_host)
    {
        return -1;
    }

    if (strlen(proxy_host) == 0 || strlen(target_host) == 0 || proxy_port <= 0 || target_port <= 0)
    {
        return -1;
    }
    int proxy_sockfd = Socket(proxy_host, proxy_port);
    if (proxy_sockfd < 0)
    {
        return -1;
    }

    // Send CONNECT request to establish connection between client and proxy.
    snprintf(connect_request, sizeof(connect_request),
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n\r\n",
            target_host, target_port, target_host, target_port);
    if (send(proxy_sockfd, connect_request, strlen(connect_request), 0) < 0)
    {
        close(proxy_sockfd);
        return -1;
    }

    // Read CONNECT response.
    ssize_t received = recv(proxy_sockfd, connect_response, sizeof(connect_response) - 1, 0);
    if (received < 0)
    {
        close(proxy_sockfd);
        return -1;
    }
    connect_response[received] = '\0';


    // Check if CONNECT succeeded.
    if (strstr(connect_response, "HTTP/1.1 200 Connection established") == NULL) {
        fprintf(stderr, "Proxy CONNECT failed: %s\n", connect_response);
        close(proxy_sockfd);
        return -1;
    }

    printf("Proxy tunnel established.\n");

    return proxy_sockfd;
}

/**
 * Regular HTTP communication.
 * 
 * RETURNS:
 *          Positive numbers: The number of bytes received from server;
 *                      zero: Sent request successfully in force mode;
 *          Negative numbers: Failed communication with server.
 */ 
static int communicate_through_http(const char *host, int port, const HTTPRequest *http_request, const int force_flg)
{
    int sockfd;
    char received_buf[8192] = {0};
    ssize_t total_received = 0;

    if (NULL == host || port <= 0)
    {
        return -1;
    }

    sockfd = Socket(host, port);

    if (sockfd < 0)
    {
        return -1;
    }

    // Send HTTP request.
    ssize_t sent = send(sockfd, http_request->body, strlen(http_request->body), 0);
    if (sent < 0)
    {
        close(sockfd);
        return -1;
    }

    // Force mode, means no need to wait for the response from server.
    if (1 == force_flg)
    {
        printf("force flag is set to 1, ignore the response from server.\n");
        close(sockfd);
        return 0;
    }
    else
    {
        // Receive response.
        ssize_t received = 0;
        while ((received = recv(sockfd, received_buf + total_received, 
                                    sizeof(received_buf) - total_received - 1, 0)) > 0)
        {
            total_received += received;
            if (total_received >= (ssize_t)(sizeof(received_buf) - 1))
            {
                break;
            }
        }
    }

    close(sockfd);
    return (int) total_received;
}

static int communicate_through_https(const char *proxy_host, const int proxy_port, const char *target_host, const int target_port, const HTTPRequest *http_request, const int force_flg)
{
    int sockfd;
    int sent;
    char received_buffer[8192] = {0};
    int received;
    
    // If the proxy is specified, then create CONNECTed sockfd for SSL tunnel.
    if (proxy_host != NULL && strlen(proxy_host) != 0)
    {
        sockfd = handle_proxy_connect(proxy_host, proxy_port, target_host, target_port);
    }
    else
    {
        // If no proxy specified, then create sockfd for SSL connect.
        sockfd = Socket(target_host, target_port);
    }

    // Initialize SSL lib
    ensure_ssl_initialized();

    // Create SSL context
    SSL_CTX *ctx = create_ssl_context();
    if (NULL == ctx)
    {
        close(sockfd);
        fprintf(stderr, "Creating SSL context failed.\n");
        return -1;
    }

    // Create SSL connection.
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sockfd);

    // Set SNI (Server Name Indication).
    SSL_set_tlsext_host_name(ssl, target_host);

    // Perform TLS handshake.
    if (SSL_connect(ssl) <= 0)
    {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sockfd);
        return -1;
    }

    printf("TLS connection established with %s:%d\n", target_host, target_port);

    // Send Request.
    sent = send_tls_data(ssl, http_request->body, strlen(http_request->body));
    if (sent < 0)
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sockfd);
        return -1;
    }
    else
    {
        printf("Sent %d bytes to server over TLS.\n", sent);
    }

    if (1 == force_flg)
    {
        printf("force flag is set to 1, ignore the response from server.\n");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sockfd);
        return 0;
    }

    received = recv_tls_data(ssl, received_buffer, sizeof(received_buffer));

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(sockfd);

    if (received < 0)
    {
        return -1;
    }
    else
    {
        printf("Received %d bytes from server over TLS.\n", received);
        return received;
    }
}

int communicate(const Arguments *args, const HTTPRequest *http_request)
{
    if (args->protocol == PROTOCOL_HTTP)
    {
        if (strlen(args->proxy_host) > 0 && args->proxy_port > 0)
        {
            // Communicate through proxy if proxy is set.
            return communicate_through_http(args->proxy_host, args->proxy_port, http_request, args->force);
        }
        else 
        {
            // Communicate directly with target host.
            return communicate_through_http(args->target_host, args->target_port, http_request, args->force);
            
        }
    }
    
    if (args->protocol == PROTOCOL_HTTPS)
    {
        return communicate_through_https(args->proxy_host, args->proxy_port, 
                                            args->target_host, args->target_port, http_request, args->force);

    }

    return 0;
}