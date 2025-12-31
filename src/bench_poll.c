#include "bench_poll.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>

#define MAX_PORT 65535

#define BUFFER_SIZE 1024
#define RECV_BUFFER_SIZE 8096
typedef enum
{
    CONN_IDLE,
    CONN_CONNECTING,
    CONN_PROXY_CONNECT,
    CONN_PROXY_RESPONSE,
    CONN_TLS_HANDSHAKE,
    CONN_SENDING,
    CONN_RECEIVING,
    CONN_COMPLETED,
    CONN_ERROR
} connection_state;

typedef struct
{
    connection_state state;
    int sockfd;
    SSL_CTX *ssl_context;
    SSL *ssl;
    bool is_https;
    const HTTPRequest *request;
    char received_response[RECV_BUFFER_SIZE];
    size_t request_len;
    int force_flag;
    size_t bytes_sent;
    size_t bytes_received;
    int speed;
    int failed;
    int bytes;
} connection;

static SSL_CTX *global_ssl_ctx = NULL;

static void init_ssl_lib(void)
{
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
}

static SSL_CTX* get_global_ssl_ctx(void)
{
    if (NULL == global_ssl_ctx)
    {
        global_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (NULL == global_ssl_ctx)
        {
            perror("Unable to create a new SSL context.");
            ERR_print_errors_fp(stderr);
            return NULL;
        }

        //@todo: Need to change to SSL_VERIFY_PEER
        SSL_CTX_set_verify(global_ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
}

static void free_ssl_lib(void)
{
    // Free SSL context if it's not NULL
    if (global_ssl_ctx != NULL)
    {
        SSL_CTX_free(global_ssl_ctx);
        global_ssl_ctx = NULL;
    }

    // Free OpenSSL lib
    EVP_cleanup();          // Free algorithm tables.
    ERR_free_strings();     // Free error message strings.
    CRYPTO_cleanup_all_ex_data();   //Additional cleanup.
}

static int create_nonblocking_socket(const char *host, const int port)
{
    if (NULL == host || (port <= 0 || port > 65535))
    {
        fprintf(stderr, "Illegal host:port specified.\n");
        return -1;
    }

    int sockfd;
    struct addrinfo hints = {
        .ai_family = AF_INET,           // IP v4
        .ai_socktype = SOCK_STREAM,     // TCP
        .ai_flags = 0,
        .ai_protocol = 0
    };
    struct addrinfo *rp, *result;
    char port_str[16] = {0};

    snprintf(port_str, sizeof(port_str), "%d", port);

    int ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        return -1;
    }

    rp = result;
    while (rp != NULL)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd <= 0)
        {
            // Socket create failed by using the current address info, try next.
            rp = rp->ai_next;
            continue;
        }

        // Set non-blocking flag on the socket.
        int flag = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);

        // Try connecting to remote, if failed, then try next address info.
        // Due to non-block socket, ignore EINPROGRESS.
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == -1 && errno != EINPROGRESS)
        {
            close(sockfd);
            rp = rp->ai_next;
            continue;
        }
        else
        {
            break;
        }
    }

    freeaddrinfo(result);
    
    if (rp == NULL)
    {
        // Means no avaliable address info can be used.
        fprintf(stderr, "Can not connect to %s:%s \n", host, port_str);
        return -1;
    }

    return sockfd;
}

static bool need_connect_proxy(const Arguments *args)
{
    return (args != NULL && strlen(args->proxy_host) && args->proxy_port > 0 && args->proxy_port <= MAX_PORT);
}

static int send_proxy_connect(connection *conn, const char *proxy_host, const int proxy_port, const char *target_host, const int target_port)
{
    if (0 == strlen(proxy_host) || 0 == strlen(target_host))
    {
        fprintf(stderr, "No proxy or target host specified.\n");
        return -1;
    }
    
    if (proxy_port <= 0 || proxy_port > MAX_PORT || target_port <= 0 || target_port > MAX_PORT)
    {
        fprintf(stderr, "Illegal proxy or target port number.\n");
        return -1;
    }
    
    if (conn == NULL || conn->sockfd <= 0)
    {
        fprintf(stderr, "No connection or socket is ready.\n");
        return -1;
    }
    
    char connect_request[BUFFER_SIZE] = {0};

    // Construct CONNECT request body and sent it to remote proxy server to establish the connection between client and proxy.
    snprintf(connect_request, sizeof(connect_request), 
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n\r\n",
            target_host, target_port, target_host, target_port);

    size_t remaining = strlen(connect_request) - conn->bytes_sent;
    if (remaining <= 0)
    {
        // The whole request has been sent, return a number which is greater than 0 to indicate no need send proxy CONNECT request any more.
        return 1;
    }
    ssize_t bytes_sent = send(conn->sockfd, connect_request + conn->bytes_sent, remaining, 0);
    if (bytes_sent <= 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Means that it is not an actually error, just need time to try again, due to it is non-block socket.
            return 0;
        }
        else
        {
            return -1;
        }
    }
    else
    {
        // Check if the whole request has been sent, or just partially sent.
        conn->bytes_sent += bytes_sent;
        if (conn->bytes_sent >= strlen(connect_request))
        {
            // Reset the bytes_sent to the next.
            conn->bytes_sent = 0;
            // The whole request has been sent.
            return 1;
        }
        else
        {
            // Partially sent the CONNECT request, continue to send the request next time.
            return 0;
        }
    }
}

static int handle_proxy_response(connection *conn)
{
    if (NULL == conn || conn->sockfd <= 0)
    {
        fprintf(stderr, "No avaliable connection.\n");
        return -1;
    }

    size_t remaining = sizeof(conn->received_response) - conn->bytes_received - 1;
    if (remaining <= 0)
    {
        conn->received_response[sizeof(conn->received_response) - 1] = '\0';
        if (strstr(conn->received_response, "HTTP/1.1 200 Connection established") == NULL)
        {
            // Means the CONNECT request is denied or failed
            return -1;
        }
        else
        {
            memset(conn->received_response, 0, sizeof(conn->received_response));
            conn->bytes_received = 0;
            return 1;
        }
    }
    
    ssize_t bytes_received = recv(conn->sockfd, conn->received_response + conn->bytes_received, remaining, 0);
    if (bytes_received > 0)
    {
        conn->bytes_received += bytes_received;
        conn->received_response[conn->bytes_received] = '\0';
        if (strstr(conn->received_response, "HTTP/1.1 200 Connection established"))
        {
            memset(conn->received_response, 0, sizeof(conn->received_response));
            conn->bytes_received = 0;
            return 1;
        }
        else if (conn->bytes_received >= sizeof(conn->received_response))
        {
            // Means the CONNECT request is denied or failed.
            return -1;
        }
        else
        {
            // Not fully received data and no 200 code return, continue to wait for next.
            return 0;
        }
    }
    else
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Means it is not an actually erro, just re-try next time.
            return 0;
        }
        else
        {
            return -1;
        }
    }
}

static int init_connection(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "NULL args for calling init_connection.\n");
        return -1;
    }

    *conn = (connection) {0};
    conn->sockfd = -1;
    conn->ssl = NULL;
    conn->is_https = (args->protocol == PROTOCOL_HTTPS);
    conn->state = CONN_IDLE;
    conn->force_flag = args->force;
    conn->speed = 0;
    conn->failed = 0;
    conn->bytes = 0;
    conn->request = http_request;
    conn->request_len = strlen(conn->request->body);
    conn->bytes_sent = 0;
    conn->bytes_received = 0; 
    return 1;
}

static int allocate_socket(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "NULL args for calling allocate_socket.\n");
        return -1;
    }

    if (need_connect_proxy(args))
    {
        conn->sockfd = create_nonblocking_socket(args->proxy_host, args->proxy_port);
    }
    else
    {
        conn->sockfd = create_nonblocking_socket(args->target_host, args->target_port);
    }

    if (conn->sockfd <= 0)
    {
        return -1;
    }
    conn->state = CONN_CONNECTING;
    return 1;
}

static void cleanup_connection(connection *conn)
{
    if (conn != NULL)
    {
        if (conn->ssl)
        {
            SSL_shutdown(conn->ssl);
            SSL_free(conn->ssl);
            conn->ssl = NULL;
        }
        if (conn->sockfd)
        {
            close(conn->sockfd);
            conn->sockfd = -1;
        }
        conn->ssl_context = NULL;
        conn->state = CONN_IDLE;
        conn->bytes_sent = 0;
        conn->bytes_received = 0;
        memset(conn->received_response, 0, sizeof(conn->received_response));
    }
}

