#include "communicator.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
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


static int Socket(const char *host, const int port)
{
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
            continue; // Try next address.
        }

        rp = rp->ai_next;
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
    if (send(proxy_sockfd, connect_request, sizeof(connect_request), 0) < 0)
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

    sockfd = Socket(host, port);

    if (sockfd < 0)
    {
        return -1;
    }

    // Send HTTP request.
    ssize_t sent = send(sockfd, http_request->body, sizeof(http_request->body), 0);
    if (sent < 0)
    {
        close(sockfd);
        return -1;
    }

    // Force mode, means no need to wait for the response from server.
    if (1 == force_flg)
    {
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

int communicator(const Arguments *args, const HTTPRequest *http_request)
{
    return 0;
}