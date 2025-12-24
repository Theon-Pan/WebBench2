#include "bench_select.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>

#define MAX_CONNECTIONS 100
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
    int request_len;
    int force_flag;
    int bytes_sent;
    int bytes_received;
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
    printf("SSL library initialized.\n");
}

static SSL_CTX *get_global_ssl_ctx(void)
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

        // @todo: Need to change to SSL_VERIFY_PEER
        SSL_CTX_set_verify(global_ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
    return global_ssl_ctx;
}

static void cleanup_ssl(void)
{
    if (global_ssl_ctx != NULL)
    {
        SSL_CTX_free(global_ssl_ctx);

        // Cleanup OpenSSL
        EVP_cleanup();                // Free algorithm tables.
        ERR_free_strings();           // Free error message strings.
        CRYPTO_cleanup_all_ex_data(); // Additional cleanup.
    }
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
        .ai_family = AF_INET,       // IPv4
        .ai_socktype = SOCK_STREAM, // TCP
        .ai_flags = 0,
        .ai_protocol = 0};
    struct addrinfo *result, *rp;
    char port_str[16] = {0};

    // Convert port number to string.
    snprintf(port_str, sizeof(port_str), "%d", port);

    // Get address info of host.
    int status = getaddrinfo(host, port_str, &hints, &result);
    if (status != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }

    rp = result;
    while (rp != NULL)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (-1 == sockfd)
        {
            continue; // Try next address.
        }

        // Set non-blocking.
        int flag = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);

        // Connect to check if the sockfd is available. If not, colse the sockfd and try the next.
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == -1 && errno != EINPROGRESS)
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
    if (NULL == rp)
    {
        fprintf(stderr, "Can not connect to %s:%d\n", host, port);
        return -1;
    }

    return sockfd;
}

static bool need_connect_proxy(const Arguments *args)
{
    return (args != NULL && strlen(args->proxy_host) && args->proxy_port > 0 && args->proxy_port < 65535);
}

static int send_proxy_connect(connection *conn, const char *proxy_host, const int proxy_port, const char *target_host, const int target_port)
{
    char connect_request[BUFFER_SIZE] = {0};
    if (0 == strlen(proxy_host) || 0 == strlen(target_host))
    {
        fprintf(stderr, "No proxy or target host specified.\n");
        return -1;
    }

    if (proxy_port <= 0 || proxy_port > 65535 || target_port <= 0 || target_port > 65535)
    {
        fprintf(stderr, "Illegal proxy or target port number.\n");
        return -1;
    }

    if (conn->sockfd < 0)
    {
        fprintf(stderr, "No socket ready.\n");
        return -1;
    }

    // Construct CONNECT request body and send it to establish the connection between client and proxy.
    snprintf(connect_request, sizeof(connect_request),
             "CONNECT %s:%d HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             target_host, target_port, target_host, target_port);

    ssize_t sent_bytes = send(conn->sockfd, connect_request, strlen(connect_request), 0);
    if (sent_bytes <= 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Not an error - just try again later.
            return 0;
        }
        return -1;
    }

    return sent_bytes;
}

static int handle_proxy_response(connection *conn)
{
    char connect_response[BUFFER_SIZE] = {0};
    ssize_t received = recv(conn->sockfd, connect_response, sizeof(connect_response) - 1, 0);
    if (received <= 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Not an error - just try again later.
            return 0;
        }
        return -1;
    }
    connect_response[received] = '\0';

    // Check if CONNECT succeeded.
    if (strstr(connect_response, "HTTP/1.1 200 Connection established") == NULL)
    {
        fprintf(stderr, "Proxy CONNECT failed: %s\n", connect_response);
        return -1;
    }

    return 1;
}

static void init_connection(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "NULL args for calling init_connection.\n");
    }

    *conn = (connection){0};
    conn->sockfd = -1;
    conn->ssl = NULL;
    conn->is_https = (args->protocol == PROTOCOL_HTTPS);
    conn->state = CONN_IDLE;
    conn->force_flag = args->force;
    conn->speed = 0;
    conn->failed = 0;
    conn->bytes = 0;
    conn->request = http_request;
    conn->request_len = strlen(http_request->body);
    conn->bytes_sent = 0;
    conn->bytes_received = 0;
}

static void allocate_socket(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "NULL args for calling allocate_socket.\n");
        exit(EXIT_FAILURE);
    }

    if (need_connect_proxy(args))
    {
        conn->sockfd = create_nonblocking_socket(args->proxy_host, args->proxy_port);
    }
    else
    {
        conn->sockfd = create_nonblocking_socket(args->target_host, args->target_port);
    }

    if (conn->sockfd < 0)
    {
        exit(EXIT_FAILURE);
    }
    conn->state = CONN_CONNECTING;
}

static void cleanup_connection(connection *conn)
{
    if (conn->ssl)
    {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->sockfd > 0)
    {
        close(conn->sockfd);
        conn->sockfd = -1;
    }
    conn->ssl_context = NULL;
    conn->state = CONN_IDLE;
    conn->bytes_sent = 0;
    conn->bytes_received = 0;
}

static void setup_connection_fdsets(connection *conn, fd_set *read_fds, fd_set *write_fds, int *max_fd)
{
    if (conn->sockfd <= 0)
    {
        return;
    }

    switch (conn->state)
    {
    case CONN_CONNECTING:
    case CONN_SENDING:
    case CONN_PROXY_CONNECT:
        FD_SET(conn->sockfd, write_fds);
        break;
    case CONN_TLS_HANDSHAKE:
        if (conn->ssl)
        {
            FD_SET(conn->sockfd, read_fds);
            FD_SET(conn->sockfd, write_fds);
        }
        break;
    case CONN_RECEIVING:
    case CONN_PROXY_RESPONSE:
        FD_SET(conn->sockfd, read_fds);
        break;
    default:
        return;
    }
    if (conn->sockfd > *max_fd)
    {
        *max_fd = conn->sockfd;
    }
}

static int handle_ready_connection(connection *conn, const Arguments *args, const HTTPRequest *http_request, fd_set *read_fds, fd_set *write_fds)
{
    if (NULL == conn || NULL == args || NULL == http_request || NULL == read_fds || NULL == write_fds)
    {
        fprintf(stderr, "Args should not be NULL.\n");
        return -1;
    }

    switch (conn->state)
    {
    case CONN_IDLE:
    case CONN_COMPLETED:
    case CONN_ERROR:
        break;
    case CONN_CONNECTING:
        if (FD_ISSET(conn->sockfd, write_fds))
        {
            // Cause it's non-block sock, so before using it, check if it's really ready.
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(conn->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
            {
                if (need_connect_proxy(args) && args->protocol == PROTOCOL_HTTPS)
                {
                    conn->state = CONN_PROXY_CONNECT;
                }
                else
                {
                    conn->state = conn->is_https ? CONN_TLS_HANDSHAKE : CONN_SENDING;

                    // If protocol is HTTPS, set SSL context.
                    if (conn->is_https)
                    {
                        conn->ssl_context = get_global_ssl_ctx();
                        if (NULL == conn->ssl_context)
                        {
                            conn->state = CONN_ERROR;
                            conn->failed++;
                            return -1;
                        }

                        conn->ssl = SSL_new(conn->ssl_context);
                        SSL_set_fd(conn->ssl, conn->sockfd);
                        SSL_set_tlsext_host_name(conn->ssl, args->target_host);
                    }
                }
            }
            else
            {
                conn->state = CONN_ERROR;
                conn->failed++;
                return -1;
            }
        }
        break;
    case CONN_PROXY_CONNECT:
        if (FD_ISSET(conn->sockfd, write_fds))
        {
            printf("Begin to establish SSL Tunnel.\n");
            int sent = send_proxy_connect(conn, args->proxy_host, args->proxy_port, args->target_host, args->target_port);
            if (sent > 0)
            {
                conn->state = CONN_PROXY_RESPONSE;
            }
            else if (sent == 0)
            {
                // Do not mean failed, just continue to sent request on next select.
                return 0;
            }
            else
            {
                conn->state = CONN_ERROR;
                conn->failed++;
                return -1;
            }
        }
        break;
    case CONN_PROXY_RESPONSE:
        if (FD_ISSET(conn->sockfd, read_fds))
        {
            int result = handle_proxy_response(conn);
            if (result == 1)
            {
                printf("SSL tunnel established.\n");
                // Proxy tunnel established, now setup SSL
                conn->state = CONN_TLS_HANDSHAKE;
                conn->ssl_context = get_global_ssl_ctx();
                if (NULL == conn->ssl_context)
                {
                    printf("Error when establishing SSL tunnel.\n");
                    conn->state = CONN_ERROR;
                    return -1;
                }
                conn->ssl = SSL_new(conn->ssl_context);
                SSL_set_fd(conn->ssl, conn->sockfd);
                SSL_set_tlsext_host_name(conn->ssl, args->target_host);
            }
            else if (result == 0)
            {
                // Do not mean failed, just continue to receive on next select.
                return 0;
            }
            else
            {
                printf("Error when establishing SSL tunnel.\n");
                conn->state = CONN_ERROR;
                conn->failed++;
                return -1;
            }
        }
        break;
    case CONN_TLS_HANDSHAKE:
        if (conn->ssl && (FD_ISSET(conn->sockfd, read_fds) || FD_ISSET(conn->sockfd, write_fds)))
        {
            //printf("Begin to TLS handshake...\n");
            int ssl_result = SSL_connect(conn->ssl);
            if (ssl_result == 1)
            {
                conn->state = CONN_SENDING;
            }
            else
            {
                int ssl_error = SSL_get_error(conn->ssl, ssl_result);
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                {
                    // Continue handshake on next select.
                    return 0;
                }
                else
                {
                    printf("Error when doing TLS handshake.\n");
                    conn->state = CONN_ERROR;
                    conn->failed++;
                    return -1;
                }
            }
            //printf("TLS handshake is done.\n");
        }
        break;
    case CONN_SENDING:
        if (FD_ISSET(conn->sockfd, write_fds))
        {
            printf("Begin to send bench request...\n");
            // If the whole request has been sent.
            int remaining = conn->request_len - conn->bytes_sent;
            if (remaining <= 0)
            {
                printf("%d bytes of bench request has been sent.[%s]\n", conn->request_len, conn->request->body);
                conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
            }
            else
            {
                int bytes_written;
                if (conn->is_https)
                {
                    if (conn->ssl)
                    {
                        // HTTPS connection.
                        bytes_written = SSL_write(conn->ssl, conn->request->body + conn->bytes_sent, remaining);
                        if (bytes_written > 0)
                        {
                            conn->bytes_sent += bytes_written;
                            // Check if all request data has been sent.
                            if (conn->bytes_sent >= conn->request_len)
                            {
                                printf("%d bytes of bench request has been sent.[%s]\n", conn->request_len, conn->request->body);
                                conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                            }
                        }
                        else
                        {
                            int ssl_error = SSL_get_error(conn->ssl, bytes_written);
                            if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ)
                            {
                                // SSL wants to write/read more, try again on the next select.
                                return 0;
                            }
                            else
                            {
                                // Real error occurred.
                                printf("Bench request sent failed.\n");
                                conn->state = CONN_ERROR;
                                conn->failed ++;
                                return -1;
                            }
                        }
                    }
                    else
                    {
                        printf("Bench request sent failed.\n");
                        conn->state = CONN_ERROR;
                        conn->failed ++;
                        return -1;
                    }
                }
                else
                {
                    // HTTP connection.
                    bytes_written = send(conn->sockfd, conn->request->body + conn->bytes_sent, remaining, 0);
                    if (bytes_written > 0)
                    {
                        conn->bytes_sent += bytes_written;
                        // Check if all request data has been sent.
                        if (conn->bytes_sent >= conn->request_len)
                        {
                            printf("%d bytes of bench request has been sent.\n", conn->request_len);
                            conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                        }
                    }
                    else if (bytes_written == -1 && ( errno == EAGAIN || errno == EWOULDBLOCK))
                    {
                        // Socket not ready, try again on the next select.
                        return 0;
                    }
                    else
                    {
                        // Real error occurred.
                        printf("Bench request sent failed.\n");
                        conn->state = CONN_ERROR;
                        conn->failed ++;
                        return -1;
                    }
                }
            }
        }
        break;
    case CONN_RECEIVING:
        if (FD_ISSET(conn->sockfd, read_fds))
        {
            int bytes_read = 0;
            int remaining_recv = sizeof(conn->received_response) - conn->bytes_received - 1;
            if (remaining_recv <= 0)
            {
                conn->state = CONN_COMPLETED;
                conn->speed ++;
            }
            if (conn->is_https)
            {
                if (conn->ssl)
                {
                    // HTTPS connection.
                    bytes_read = SSL_read(conn->ssl, conn->received_response + conn->bytes_received, remaining_recv);
                    if (bytes_read > 0)
                    {
                        conn->bytes_received += bytes_read;
                        conn->received_response[conn->bytes_received] = '\0';

                        // Check for the end of HTTP headers.
                        if (strstr(conn->received_response, "\r\n\r\n"))
                        {
                            // Headers complete.
                            printf("%d bytes of response received.[%s]\n", conn->bytes_received, conn->received_response);
                            conn->state = CONN_COMPLETED;
                            conn->bytes += conn->bytes_received;
                            conn->speed ++;
                        }
                        else
                        {
                            // Continue to read.
                            return 0;
                        }
                    }
                    else 
                    {
                        int ssl_error = SSL_get_error(conn->ssl, bytes_read);
                        if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ)
                        {
                            // SSL wants to read or write more, try again on the next select.
                            return 0;
                        }
                        else
                        {
                            // Real error occurred.
                            printf("Bench response received failed.\n");
                            conn->state = CONN_ERROR;
                            conn->failed ++;
                            return -1;
                        }
                    }

                }
                else
                {
                    printf("Bench request received failed.\n");
                    conn->state = CONN_ERROR;
                    conn->failed ++;
                    return -1;
                }

            }
            else
            {
                // HTTP connection.
                bytes_read = recv(conn->sockfd, conn->received_response + conn->bytes_received, remaining_recv, 0);
                if (bytes_read > 0)
                {
                    conn->bytes_received += bytes_read;
                    conn->received_response[conn->bytes_received] = '\0';

                    // Check if meeting the end of HTTP headers.
                    if (strstr(conn->received_response, "\r\n\r\n"))
                    {
                        printf("%d bytes of response received.\n", conn->bytes_received);
                        // Headers received completly.
                        conn->state = CONN_COMPLETED;
                        conn->bytes += conn->bytes_received;
                        conn->speed ++;
                    }
                    else
                    {
                        // Continue to read on the next select.
                        return 0;
                    }
                }
                else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    // Socket not ready, try again on the next select.
                    return 0;
                }
                else
                {
                    // Real error occurred.
                    printf("%d bytes of response received.\n", conn->bytes_received);
                    conn->state = CONN_ERROR;
                    conn->failed ++;
                    return -1;
                }
            }

        }
        break;
    }

    return 0;
}

void bench_select(const Arguments *args, const HTTPRequest *http_request)
{
    int num_connections;
    time_t start_time = time(NULL);
    fd_set read_fds, write_fds;
    int max_fd = 0;
    struct timeval select_timeout = {0, 100000}; // 100ms timeout

    if (NULL == args || NULL == http_request)
    {
        fprintf(stderr, "No args or request to bench.\n");
        return;
    }

    num_connections = args->clients;
    if (num_connections > MAX_CONNECTIONS)
    {
        num_connections = MAX_CONNECTIONS;
        printf("Warning: Limited to %d connections to server.\n", num_connections);
    }

    connection *connections = (connection *)malloc(num_connections * sizeof(connection));
    if (NULL == connections)
    {
        perror("Memory allocation for connections is failed");
        return;
    }

    printf("Starting to bench with %d connection/connections...\r\n", num_connections);

    // Initialize all connections.
    for (int i = 0; i < num_connections; i++)
    {
        init_connection(args, http_request, &connections[i]);
        allocate_socket(args, http_request, &connections[i]);
    }

    // If the protocol is HTTPS, initialize the SSL library.
    if (args->protocol == PROTOCOL_HTTPS)
    {
        init_ssl_lib();
    }

    // Execute bench within the specified time range.
    while (time(NULL) - start_time <= args->bench_time)
    {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        // Setup connections to fd set based on current states.
        for (int i = 0; i < num_connections; i++)
        {
            // For the connection which complete one communication with server or encounter error,
            // cleanup the original connection and re-connect for next.
            if (connections[i].state == CONN_COMPLETED || connections[i].state == CONN_ERROR)
            {
                cleanup_connection(&connections[i]);
                allocate_socket(args, http_request, &connections[i]);
            }
            setup_connection_fdsets(&connections[i], &read_fds, &write_fds, &max_fd);
        }

        if (max_fd > 0)
        {
            // Wait for events.
            int ready = select(max_fd + 1, &read_fds, &write_fds, NULL, &select_timeout);
            if (ready > 0)
            {
                // handle the ready fds.
                for (int i = 0; i < num_connections; i++)
                {
                    handle_ready_connection(&connections[i], args, http_request, &read_fds, &write_fds);
                }
            }
        }
    }
    
    // Release all sockets, ssl and ssl context, free the memory for connections array, summary the results.
    int total_failed = 0;
    int total_bytes = 0;
    int total_speed = 0;
    if (connections != NULL)
    {
        for (int i = 0; i < num_connections; i++)
        {
            total_failed += connections[i].failed;
            total_speed += connections[i].speed;
            total_bytes += connections[i].bytes;
            cleanup_connection(&connections[i]);
        }
        cleanup_ssl();
        free(connections);
    }

    printf("Bench select is done. speed=[%d], bytes=[%d], failed=[%d].\n", total_speed, total_bytes, total_failed);
    
}