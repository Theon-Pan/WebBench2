#include "bench_poll.h"
#include "bitmap.h"
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
#include <poll.h>
#include <string.h>

#define MAX_PORT 65535

#define BUFFER_SIZE 1024
#define RECV_BUFFER_SIZE 8096

#define MAX_CONNECTIONS 1000
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
    return global_ssl_ctx;
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

static int setup_ssl_context(connection *conn)
{
    if (NULL == conn)
    {
        return -1;
    }

    conn->ssl_context = get_global_ssl_ctx();
    if (NULL == conn->ssl_context)
    {
        return -1;
    }

    conn->ssl = SSL_new(conn->ssl_context);
    if (NULL == conn->ssl)
    {
        return -1;
    }

    if (0 == SSL_set_fd(conn->ssl, conn->sockfd))
    {
        return -1;
    }

    return 1;
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

static int setup_connection_fdsets(connection *conn, const int num_connections, const Arguments *args, const HTTPRequest *http_request, struct pollfd *poll_fd, char *conn_setup_bitmap, int bitmap_size)
{
    if (NULL == conn || conn->sockfd <= 0)
    {
        return -1;
    }

    if (NULL == args || NULL == http_request)
    {
        return -1;
    }

    if (NULL == poll_fd)
    {
        return -1;
    }

    if (NULL == conn_setup_bitmap || bitmap_size <= 0)
    {
        return -1;
    }

    connection *curr_conn = conn;
    struct pollfd *curr_poll_fd = poll_fd;
    int poll_fds_num = 0;
    for (int i = 0; i < num_connections; i++)
    {
        switch(curr_conn->state)
        {
            case CONN_ERROR:
            case CONN_COMPLETED:
                cleanup_connection(curr_conn);
                allocate_socket(args, http_request, curr_conn);
                break;
            case CONN_CONNECTING:
            case CONN_SENDING:
            case CONN_PROXY_CONNECT:
                if (set_bitmap(i, conn_setup_bitmap, bitmap_size) < 0)
                {
                    return -1;
                }
                curr_poll_fd->fd = curr_conn->sockfd;
                curr_poll_fd->events = POLLOUT;
                curr_poll_fd->revents = 0;
                curr_poll_fd ++;
                poll_fds_num ++;
                break;
            case CONN_RECEIVING:
            case CONN_PROXY_RESPONSE:
                if (set_bitmap(i, conn_setup_bitmap, bitmap_size) < 0)
                {
                    return -1;
                }
                curr_poll_fd->fd = curr_conn->sockfd;
                curr_poll_fd->events = POLLIN;
                curr_poll_fd->revents = 0;
                curr_poll_fd ++;
                poll_fds_num ++;
                break;
            case CONN_TLS_HANDSHAKE:
                // For TLS handshake, we might need to read or write
                // The specific direction depends on SSL_get_error() 's result.
                if (set_bitmap(i, conn_setup_bitmap, sizeof(conn_setup_bitmap)) < 0)
                {
                    return -1;
                }
                curr_poll_fd->fd = curr_conn->sockfd;
                curr_poll_fd->events = POLLIN | POLLOUT;    // Monitor both directions.
                curr_poll_fd->revents = 0;
                curr_poll_fd ++;
                poll_fds_num ++;
                break;
            case CONN_IDLE:
                break;

        }
        curr_conn ++;
    }
    return poll_fds_num;
}

static int handle_ready_connection(connection *conn, const Arguments *args, struct pollfd *pfd)
{
    if (NULL == conn || NULL == args || NULL == pfd)
    {
        fprintf(stderr, "Args of handle_ready_connection should not be NULL.\n");
        return -1;
    }

    // Check for errors first.
    if (pfd->revents &(POLLERR | POLLHUP | POLLNVAL))
    {
        conn->failed++;
        conn->state = CONN_ERROR;
        return -1;
    }

    switch(conn->state)
    {
        case CONN_CONNECTING:
            if (pfd->revents & POLLOUT)
            {
                // Cause it is non-block sock, so before using it, check if it is really ready.
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(conn->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                {
                    if (need_connect_proxy(args) && conn->is_https)
                    {
                        // Create SSL tunnel, if access remote through TLS.
                        conn->state = CONN_PROXY_CONNECT;
                    }
                    else
                    {
                        conn->state = conn->is_https ? CONN_TLS_HANDSHAKE : CONN_SENDING;

                        // If protocol is HTTPS, set SSL context.
                        if (conn->is_https)
                        {
                            if (setup_ssl_context(conn) > 0)
                            {
                                SSL_set_tlsext_host_name(conn->ssl, args->target_host);
                            }
                            else
                            {
                                fprintf(stderr, "Error when setting the SSL context on connection.\n");
                                conn->state = CONN_ERROR;
                                conn->failed++;
                                return -1;
                            }
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
            if (pfd->revents & POLLOUT)
            {
                printf("Begin to establish SSL tunnel.\n");
                int sent = send_proxy_connect(conn, args->proxy_host, args->proxy_port, args->target_host, args->target_port);
                if (sent > 0)
                {
                    conn->state = CONN_PROXY_RESPONSE;
                }
                else if (0 == sent)
                {
                    // Not fully sent, continue to send request on the next poll.
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
        case CONN_PROXY_RESPONSE:
            if (pfd->revents & POLLIN)
            {
                int recv = handle_proxy_response(conn);
                if (1 == recv)
                {
                    printf("SSL tunnel established.\n");

                    // Proxy tunnel established, now setup SSL
                    setup_ssl_context(conn);
                    conn->state = CONN_TLS_HANDSHAKE;
                }
                else if (0 == recv)
                {
                    // Means not fully data received, need to continue to receive data in the next poll.
                    return 0;
                }
                else
                {
                    printf("Error when establishing SSL tunnel.\n");
                    conn->state = CONN_ERROR;
                    conn->failed++;
                }

            }
            break;
        case CONN_TLS_HANDSHAKE:
            if (conn->ssl != NULL && pfd->revents & (POLLIN | POLLOUT))
            {
                //printf("Begin to TLS handshake...\n");
                int handshake_result = SSL_connect(conn->ssl);
                if (handshake_result == 1)
                {
                    // TLS/SSL handshake is successful.
                    conn->state = CONN_SENDING;
                }
                else
                {
                    int handshake_error = SSL_get_error(conn->ssl, handshake_result);
                    if (handshake_error == SSL_ERROR_WANT_READ || handshake_error == SSL_ERROR_WANT_WRITE)
                    {
                        // Not an actually error, continue handshake in the next poll.
                        return 0;
                    }
                    else
                    {
                        fprintf(stderr, "Error when doing TLS handshake.\n");
                        conn->state = CONN_ERROR;
                        conn->failed++;
                        return -1;
                    }
                }
            }
            break;
        case CONN_SENDING:
            if (pfd->revents & POLLOUT)
            {
                printf("Begin to send bench request...\n");
                int remaining = conn->request_len - conn->bytes_sent;
                if (remaining <= 0)
                {
                    // The whole request has been sent.
                    printf("%ld bytes of bench request has been sent.[%s]\n", conn->request_len, conn->request->body);
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
                                if (conn->bytes_sent >= conn->request_len)
                                {
                                    // The whole request has been sent.
                                    printf("%ld bytes of bench request has been sent.[%s]\n", conn->request_len, conn->request->body);
                                    conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                                }
                            }
                            else
                            {
                                // Need to check if it just need to retry in the next cycle.
                                int ssl_error = SSL_get_error(conn->ssl, bytes_written);
                                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                                {
                                    // Just need to retry in the next cycle.
                                    return 0;
                                }
                                else
                                {
                                    // Real error occurred.
                                    fprintf(stderr, "Bench request sent failed.\n");
                                    conn->state = CONN_ERROR;
                                    conn->failed++;
                                    return -1;
                                }
                            }
                        }
                        else
                        {
                            fprintf(stderr, "Bench request sent failed.\n");
                            conn->state = CONN_ERROR;
                            conn->failed++;
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
                            // Check if the whole request data has been sent.
                            if (conn->bytes_sent >= conn->request_len)
                            {
                                printf("%ld bytes of bench request has been sent.[%s]\n", conn->request_len, conn->request->body);
                                conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                            }
                        }
                        else if (bytes_written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // Socket is not ready for writing, try again in the next cycle.
                            return 0;
                        }
                        else
                        {
                            // Real error occurred.
                            fprintf(stderr, "Bench request sent failed.\n");
                            conn->state = CONN_ERROR;
                            conn->failed++;
                            return -1;
                        }
                    }
                }
            }
            break;
        case CONN_RECEIVING:
            if (pfd->revents & POLLIN)
            {
                int remaining_recv = sizeof(conn->received_response) - conn->bytes_received - 1;
                if (remaining_recv <= 0)
                {
                    conn->state = CONN_COMPLETED;
                    conn->speed++;
                }
                else
                {
                    int bytes_read = 0;
                    if (conn->is_https)
                    {
                        bytes_read = SSL_read(conn->ssl, conn->received_response + conn->bytes_received, remaining_recv);
                        if (bytes_read > 0)
                        {
                            conn->bytes_received += bytes_read;
                            conn->received_response[conn->bytes_received] = '\0';

                            // Check if meets the end of HTTP headers.
                            if (strstr(conn->received_response, "\r\n\r\n"))
                            {
                                // Headers is fully received, complete this communication.
                                printf("%ld bytes of response is received.[%s]\n", conn->bytes_received, conn->received_response);
                                conn->state = CONN_COMPLETED;
                                conn->bytes += conn->bytes_received;
                                conn->speed++;
                            }
                            else
                            {
                                // Headers is not fully received. continue to receive in the next cycle.
                                return 0;
                            }
                        }
                        else
                        {
                            int ssl_error = SSL_get_error(conn->ssl, bytes_read);
                            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                            {
                                // Not a real error, try again in the next cycle.
                                return 0;
                            }
                            else
                            {
                                // Real error occurred.
                                fprintf(stderr, "Bench response receiving is failed.\n");
                                conn->state = CONN_ERROR;
                                conn->failed++;
                                return -1;
                            }
                        }
                    }
                    else
                    {
                        // HTTP connection.
                        bytes_read = read(conn->sockfd, conn->received_response + conn->bytes_received, remaining_recv);
                        if (bytes_read > 0)
                        {
                            conn->bytes_received += bytes_read;
                            conn->received_response[conn->bytes_received] = '\0';

                            // Check for the end of HTTP headers.
                            if (strstr(conn->received_response, "\r\n\r\n"))
                            {
                                // Headers is fully received.
                                printf("%ld bytes of response received.[%s]\n", conn->bytes_received, conn->received_response);
                                conn->state = CONN_COMPLETED;
                                conn->speed++;
                                conn->bytes += conn->bytes_received;
                            }
                            else
                            {
                                // Headers is not fully received, continue to receive in the next cycle.
                                return 0;
                            }

                        }
                        else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // Socket not ready, try again in the next cycle.
                            return 0;
                        }
                        else
                        {
                            // Real error occurred.
                            fprintf(stderr, "Bench response receiving is failed.\n");
                            conn->state = CONN_ERROR;
                            conn->failed++;
                            return -1;
                        }
                    }
                }
            }
            break;
        default:
            break;
    }

    return 0;
}

void bench_poll(const Arguments *args, const HTTPRequest *http_request)
{
    if (NULL == args || NULL == http_request)
    {
        fprintf(stderr, "No args or request to bench.\n");
        return;
    }

    int num_connections;
    time_t start_time;

    num_connections = args->clients;

    if (num_connections > MAX_CONNECTIONS)
    {
        num_connections = MAX_CONNECTIONS;
        printf("Warning: Limited to %d connections to server.\n", num_connections);
    }

    connection *connections =  (connection *) malloc(sizeof(connection) * num_connections);
    if (NULL == connections)
    {
        perror("Memory allocation for connections is failed.");
        return;
    }

    printf("Starting to bench with %d connection/connections...\n", num_connections);

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
    start_time = time(NULL);

    // Setup bitmap for tracking which connection is set in te poll fds array.
    unsigned short bitmap_size = num_connections / (sizeof(char) * 8);
    if (num_connections % (sizeof(char) * 8) != 0)
    {
        bitmap_size += 1;
    }

    char bitmap[bitmap_size];
    while (time(NULL) - start_time <= args->bench_time)
    {
        struct pollfd pollfds[num_connections];

        // Setup connections to pollfds based on the current state.
        memset(bitmap, 0, bitmap_size);
        int poll_fds_num = setup_connection_fdsets(connections, num_connections, args, http_request, pollfds, bitmap, bitmap_size);
        if (poll_fds_num > 0)
        {
            int ready = poll(pollfds, poll_fds_num, 100);
            if (ready < 0)
            {
                perror("Poll return negetive");
                break;
            }
            if (ready == 0)
            {
                // Poll time out, continue the loop.
                printf("Poll timeout, go next loop...\n");
                continue;
            }

            // Handle the ready fds.
            connection *curr_conn = connections;
            struct pollfd *curr_poll_fd = pollfds;
            for (int i = 0; i < num_connections; i++)
            {
                // Find the corresponding connection for this pollfd.
                if (get_bitmap(i, bitmap, bitmap_size))
                {
                    handle_ready_connection(curr_conn, args, curr_poll_fd);
            
                    curr_poll_fd++;
                }
                curr_conn++;
            }
        }
        usleep(10000);
    }

    // Release all connections, ssl and ssl context, free the memory allocated for connections array, summary the results.
    int total_failed = 0;
    int total_speed = 0;
    int total_bytes = 0;
    if (connections != NULL)
    {
        for (int i = 0; i < num_connections; i++)
        {
            total_failed += connections[i].failed;
            total_speed += connections[i].speed;
            total_bytes += connections[i].bytes;
            cleanup_connection(&connections[i]);
        }
        free(connections);
    }
    if (args->protocol == PROTOCOL_HTTPS)
    {
        free_ssl_lib();
    }

    printf("Bench poll is done. speed=[%d], bytes=[%d], failed=[%d].\n", total_speed, total_bytes, total_failed);
    
}

