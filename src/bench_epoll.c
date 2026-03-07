#include "bench_epoll.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <bitmap.h>
#include <unistd.h>

#define MAX_CONNECTIONS 1000
#define RECV_BUFFER_SIZE 8096
#define BUFFER_SIZE 1024

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

        //@todo: Need to change to SSL_VERIFY_PEER.
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
    CRYPTO_cleanup_all_ex_data();   // Additional cleanup.
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

static bool need_connect_proxy(const Arguments *args)
{
    return (strlen(args->proxy_host) && (args->proxy_port > 0 && IS_VALID_PORT(args->proxy_port)));
}

static int send_proxy_connect(connection *conn, const char *proxy_host, const int proxy_port, const char *target_host, const int target_port)
{
    if (0 == strlen(proxy_host) || 0 == strlen(target_host))
    {
        fprintf(stderr, "No proxy or target host specified.\n");
        return -1;
    }

    if(!IS_VALID_PORT(proxy_port) || !IS_VALID_PORT(target_port))
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
    // Construct CONNECT request body and send it to remote proxy server to establish the connection between client and proxy.
    snprintf(connect_request, sizeof(connect_request),
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Connection: close\r\n\r\n",
            target_host, target_port, target_host, target_port);
    
    size_t remaining = strlen(connect_request) - conn->bytes_sent;
    if (remaining <= 0)
    {
        // The whole request has been sent, return a number which is greater than 0 to indicate no need to send proxy CONNECT request anymore.
        return 1;
    }

    size_t bytes_sent = send(conn->sockfd, connect_request + conn->bytes_sent, remaining, 0);
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
    if (NULL == conn || conn->sockfd < 0)
    {
        fprintf(stderr, "No connection or socket is ready!\n");
        return -1;
    }

    size_t remaining = sizeof(conn->received_response) - conn->bytes_received - 1;
    if (remaining <= 0)
    {
        conn->received_response[sizeof(conn->received_response) - 1] = '\0';
        if (strstr(conn->received_response, "HTTP/1.1 200 Connection established") == NULL)
        {
            // Means the CONNECT request is denied or failed.
            return -1;
        }
        else
        {
            // The CONNECT request is responded by proxy successfully.
            // Reset the receive buffer.
            memset(conn->received_response, 0, sizeof(conn->received_response));
            // Reset the received bytes count.
            conn->bytes_received = 0;
            return 1;
        }
    }

    ssize_t bytes_received = recv(conn->sockfd, conn->received_response + conn->bytes_received, remaining, 0);
    if (bytes_received > 0)
    {
        conn->bytes_received += bytes_received;
        conn->received_response[conn->bytes_received] = '\0';
        if(strstr(conn->received_response, "HTTP/1.1 200 Connection established"))
        {
            // The CONNECT request is responded by proxy successfully.
            // Reset the receive buffer.
            memset(conn->received_response, 0, sizeof(conn->received_response));
            // Reset the received bytes count.
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
            // Not fully received response data and no 200 code return, continue to wait for next.
            return 0;
        }
    }
    else
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Means it is not an acutally error, just re-try next time.
            return 0;
        }
        else
        {
            return -1;
        }
    }


}

static int create_nonblocking_socket(const char *host, const int port)
{
    if (NULL == host || !IS_VALID_PORT(port))
    {
        fprintf(stderr, "Illegal host:port \n");
        return -1;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,       // IP v4
        .ai_socktype = SOCK_STREAM, // TCP
        .ai_flags = 0,
        .ai_protocol = 0
    };
    struct addrinfo *rp, *result;
    int sockfd;

    char port_str[16] = {0};
    snprintf(port_str, 16, "%d", port);
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

        // Set non-blocking flag to the socket.
        int flag = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);

        // Try connecting to the remote, if failed, try next address info.
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
    
    if (NULL == rp)
    {
        // Means no available address info can ben used.
        fprintf(stderr, "Can not connect to %s:%s \n", host, port_str);
        return -1;
    }

    return sockfd;
}

static int init_connection(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "Args of calling init_connection is NULL.\n");
        return -1;
    }

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
        if (conn->sockfd > 0)
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

static int allocate_socket(const Arguments *args, const HTTPRequest *http_request, connection *conn)
{
    if (NULL == args || NULL == http_request || NULL == conn)
    {
        fprintf(stderr, "Args of calling allocate_socket is NULL.\n");
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



static int setup_connection_to_epoll_instance(connection *conn, const int num_connections, 
            const Arguments *args, const HTTPRequest *http_request, const int epoll_fd)
{
    if (NULL == conn || conn->sockfd < 0 || num_connections <= 0)
    {
        return -1;
    }

    if (NULL == args || NULL == http_request)
    {
        return -1;
    }

    if (epoll_fd < 0)
    {
        return -1;
    }

    connection *curr_conn = conn;
    int epoll_fds_num = 0;
    for (int i = 0; i < num_connections; i++, curr_conn++)
    {
        struct epoll_event event = {0};

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
                event.data.ptr = curr_conn;
                event.events = EPOLLOUT | EPOLLET;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, curr_conn->sockfd, &event) == -1)
                {
                    if (errno != EEXIST)
                    {
                        perror("OUT: epoll_ctl ADD");
                        return -1;
                    }
                }
                epoll_fds_num++;
                break;
            case CONN_RECEIVING:
            case CONN_PROXY_RESPONSE:
                event.data.ptr = curr_conn;
                event.events = EPOLLIN | EPOLLET;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, curr_conn->sockfd, &event) == -1)
                {
                    if (errno != EEXIST)
                    {
                        perror("IN: epoll_ctl ADD");
                        return -1;
                    }
                }
                epoll_fds_num++;
                break;
            case CONN_TLS_HANDSHAKE:
                // For TLS handshake, we might need to read or write.
                event.data.ptr = curr_conn;
                event.events = EPOLLIN | EPOLLOUT | EPOLLET;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, curr_conn->sockfd, &event) == -1)
                {
                    if (errno != EEXIST)
                    {
                        perror("TLS_HANDSHAKE: epoll_ctl ADD");
                        return -1;
                    }
                }
                epoll_fds_num++;
                break;
            case CONN_IDLE:
                break;
        }
    }
    return epoll_fds_num;
}

static int handle_ready_connection(const Arguments *args, const struct epoll_event *event, const int epoll_fd)
{
    if (event == NULL || epoll_fd < 0)
    {
        return -1;
    }

    connection *conn = (connection *) event->data.ptr;
    uint32_t ev = event->events;

    // Remove from epoll (will be re-added next iterationif needed)
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->sockfd, NULL) == -1)
    {
        perror("epoll_ctl DEL failed.");
        return -1;
    }

    // If the event is Error.
    if (ev & (EPOLLERR | EPOLLHUP))
    {
        conn->state = CONN_ERROR;
        conn->failed++;
        return -1;
    }

    switch(conn->state)
    {
        case CONN_CONNECTING:
            // Handle connection establishment.
            if (ev & EPOLLOUT)
            {
                // Because it's non-block socket so before using it, check if it's really ready.
                printf("Begin to connect...\n");
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(conn->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                {
                    // No error, means the connection is established successfully.
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
            if (ev & EPOLLOUT)
            {
                printf("Begin to establish SSL tunnel...\n");
                int sent = send_proxy_connect(conn, args->proxy_host, args->proxy_port, args->target_host, args->target_port);
                if (sent > 0)
                {
                    printf("CONNECT request is sent to proxy, waiting for its response...\n");
                    conn->state = CONN_PROXY_RESPONSE;
                }
                else if (0 == sent)
                {
                    // Not fully sent, continue to send request on the next epoll.
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
            if (ev & EPOLLIN)
            {
                int recv = handle_proxy_response(conn);
                if (1 == recv)
                {
                    printf("SSL tunnel is established.\n");

                    // Proxy tunnel is established, now set SSL up.
                    setup_ssl_context(conn);
                    conn->state = CONN_TLS_HANDSHAKE;
                }
                else if (0 == recv)
                {
                    // Means not fully received data, need to contine to receive data in the next poll.
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
            if (conn->ssl != NULL && (ev & (EPOLLIN | EPOLLOUT)))
            {
                printf("Begin to TLS handshake...\n");
                int handshake_result = SSL_connect(conn->ssl);
                if (1 == handshake_result)
                {
                    // TLS handshake runs successfully.
                    conn->state = CONN_SENDING;
                    return 1;
                }
                else
                {
                    int handshake_error = SSL_get_error(conn->ssl, handshake_result);
                    if (handshake_error == SSL_ERROR_WANT_READ || handshake_error == SSL_ERROR_WANT_WRITE)
                    {
                        // Not actual error, continue to handshake in the next poll.
                        return 0;
                    }
                    else
                    {
                        // Occurred error during TLS handshake.
                        fprintf(stderr, "Occurred error during TLS handshake.\n");
                        conn->state = CONN_ERROR;
                        conn->failed++;
                        return -1;
                    }
                }
            }
            break;
        case CONN_SENDING:
            if (ev & EPOLLOUT)
            {
                printf("Begin to send bench request...\n");
                int remaining = conn->request_len - conn->bytes_sent;
                if (remaining <= 0)
                {
                    // The whole request has benn sent.
                    printf("%ld bytes of bench request has been sent.\n[%s]\n", conn->request_len, conn->request->body);
                    conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                }   
                else
                {
                    int bytes_written;
                    if (conn->is_https)
                    {
                        // HTTPS connection.
                        if (conn->ssl)
                        {
                            bytes_written = SSL_write(conn->ssl, conn->request->body + conn->bytes_sent, remaining);
                            if (bytes_written > 0)
                            {
                                conn->bytes_sent += bytes_written;
                                if (conn->bytes_sent >= conn->request_len)
                                {
                                    // The whole request has been sent.
                                    printf("%ld bytes of bench request has benn sent.\n[%s]\n", conn->bytes_sent, conn->request->body);
                                    conn->state = conn->force_flag ? CONN_COMPLETED : CONN_RECEIVING;
                                }
                            }
                            else
                            {
                                // Need to check if it just need to re-try in the next cycle.
                                int ssl_error = SSL_get_error(conn->ssl, bytes_written);
                                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                                {
                                    // Just need to re-try in the next cycle.
                                    return 0;
                                }
                                else
                                {
                                    // Actual error occurred.
                                    fprintf(stderr, "Failed to send bench request.\n");
                                    conn->state = CONN_ERROR;
                                    conn->failed++;
                                    return -1;
                                }
                            }
                        }
                        else
                        {
                            fprintf(stderr, "Failed to send bench request due to the failed ssl initialization.\n");
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
                                printf("%ld bytes of bench request has been sent.\n[%s]\n", conn->bytes_sent, conn->request->body);
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
                            // Actual error occurred.
                            fprintf(stderr, "Failed to send bench request.\n");
                            conn->state = CONN_ERROR;
                            conn->failed++;
                            return -1;
                        }
                    }
                }
            }
            break;
        case CONN_RECEIVING:
            if (ev & EPOLLIN)
            {
                int remaining_recv = sizeof(conn->received_response) - conn->bytes_received - 1;
                if (remaining_recv <= 0)
                {
                    conn->state = CONN_COMPLETED;
                    conn->speed++;
                    conn->bytes += conn->bytes_received;
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
                                // HTTP headers is fully received, complete the connection.
                                printf("%ld bytes of response is received.\n[%s]\n", conn->bytes_received, conn->received_response);
                                conn->state = CONN_COMPLETED;
                                conn->bytes += conn->bytes_received;
                                conn->speed++;
                            }
                            else
                            {
                                // Headers is not fully received, continue to receive in the next cycle.
                                return 0;
                            }
                        }
                        else
                        {
                            int ssl_error = SSL_get_error(conn->ssl, bytes_read);
                            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE)
                            {
                                // Not an actual error, try again in the next cycle.
                                return 0;
                            }
                            else
                            {
                                // An actual error occurred.
                                fprintf(stderr, "Failed to receive bench response.\n");
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

                            // Check if meets the end of HTTP readers.
                            if (strstr(conn->received_response, "\r\n\r\n"))
                            {
                                // HTTP headers is fully received, complete the connection.
                                printf("%ld bytes of response is received.\n[%s]\n", conn->bytes_received, conn->received_response);
                                conn->state = CONN_COMPLETED;
                                conn->bytes += conn->bytes_received;
                                conn->speed++;
                            }
                            else
                            {
                                // HTTP headers is not fully received, continue to receive in the next cycle.
                                return 0;
                            }
                        }
                        else if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            // Socket is not ready, try again in the next cycle.
                            return 0;
                        }
                        else
                        {
                            // Actual error occurred.
                            fprintf(stderr, "Failed to receive bench response.\n");
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

void bench_epoll(const Arguments *args, const HTTPRequest *http_request)
{
    if (NULL == args || NULL == http_request)
    {
        fprintf(stderr, "No args or request to bench.\n");
        return;
    }

    int num_connections = args->clients;
    time_t start_time;
    int epfd;
    struct epoll_event events[num_connections];

    if (num_connections > MAX_CONNECTIONS)
    {
        num_connections = MAX_CONNECTIONS;
        printf("Warning: Limited to %d connections to the remote server.\n", num_connections);
    }

    // Allocate memory for connections.
    connection *connections = (connection *) calloc(num_connections, sizeof(connection));
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

    // If the protocal is HTTPS, initialize the SSL library first.
    if (args->protocol == PROTOCOL_HTTPS)
    {
         init_ssl_lib();
    }

    // Create epoll instance.
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
    {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    // Execute bench within the specified time range.
    start_time = time(NULL);
    while (time(NULL) - start_time <= args->bench_time)
    {
        int active_fds = setup_connection_to_epoll_instance(connections, num_connections, args, http_request, epfd);
       
        if (active_fds < 0)
        {
            exit(EXIT_FAILURE);
        }
        else if (active_fds == 0)
        {
            continue;
        }

        // Wait for events.
        int nfds = epoll_wait(epfd, events, active_fds, 100);
        if (nfds == -1)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            handle_ready_connection(args, &events[i], epfd);
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
    if(args->protocol == PROTOCOL_HTTPS)
    {
        free_ssl_lib();
    }

    printf("Bench epoll is done. speed=[%d], bytes=[%d], failed[%d].\n", total_speed, total_bytes, total_failed);

}