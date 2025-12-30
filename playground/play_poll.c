#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    CONN_CONNECTING,
    CONN_SENDING,
    CONN_RECEIVING,
    CONN_ERROR,
    CONN_COMPLETED
} connection_state;

int create_nonblocking_socket(const char *host, const char *service)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = 0,
        .ai_protocol = 0
    };
    
    struct addrinfo *rp, *result;
    int sockfd = -1;
    
    int ret = getaddrinfo(host, service, &hints, &result);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo erro: %s \n", gai_strerror(ret));
        return -1;
    }
    
    rp = result;
    while (rp != NULL)
    {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd <= 0)
        {
            perror("Create socket failed, try next...");
            rp = rp->ai_next;
            continue;
        }
    
        // Set sock non-blocking.
        int flag = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flag | O_NONBLOCK);
    
        struct sockaddr_in *address = (struct sockaddr_in *) rp->ai_addr;
        
        char ip_addr[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(address->sin_addr), ip_addr, sizeof(ip_addr));
    
        printf("Connect to %s:%d...\n", ip_addr, ntohs(address->sin_port));
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) != 0 && errno != EINPROGRESS)
        {
            fprintf(stderr, "Cannot connect to %s:%d, try next...\n", ip_addr, ntohs(address->sin_port));
            rp = rp->ai_next;
            continue;
        }
        break;
    }
    
    if (rp == NULL && sockfd == -1){
        fprintf(stderr, "Cannot connect to %s with service %s.\n", host, service);
        return -1;
    }
    return sockfd;
}

int main(int argc, char *argv[])
{
    // if (argc != 3)
    // {
    //     printf("Usage: %s <http/https> <host>\n", argv[0]);
    //     printf("Example: %s https www.google.com\n", argv[0]);
    //     return 1;
    // }

    // const char *host = argv[2];
    // const char *service = argv[1];
    const char *host = "localhost";
    const char *service = "4000";
    int sockfd = create_nonblocking_socket(host, service);
    if (sockfd < 0)
    {
        exit(EXIT_FAILURE);
    }


    time_t start_time = time(NULL);

    char request[1024] = {0};
    snprintf(request, sizeof(request),
            "GET / HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "User-Agent: poll-example/1.0\r\n"
            "\r\n", host);
    
    char response[8096] = {0};
    

    struct pollfd pollfds[1];
    pollfds[0].fd = sockfd;
    // pollfds[0].fd = sockfd;
    // pollfds[0].events = POLLOUT;     // Initially wait for connection.
    // pollfds[0].revents = 0;

    connection_state conn_state = CONN_CONNECTING;
    int bytes_sent = 0;
    int bytes_received = 0;
    while (time(NULL) - start_time < 300)
    {
        // Setup the poll fds
        switch (conn_state)
        {
            case CONN_ERROR:
            case CONN_COMPLETED:
                if (pollfds[0].fd > 0)
                {
                    close(pollfds[0].fd);
                }

                pollfds[0].fd = create_nonblocking_socket(host, service);
                if (pollfds[0].fd <=  0)
                {
                    continue;
                }
                pollfds[0].events = POLLOUT;
                pollfds[0].revents = 0;
                bytes_sent = bytes_received = 0;
                conn_state = CONN_CONNECTING;
                break;
                
            case CONN_CONNECTING:
            case CONN_SENDING:
                pollfds[0].events = POLLOUT;
                pollfds[0].revents = 0;
                break;
            case CONN_RECEIVING:
                pollfds[0].events = POLLIN;
                pollfds[0].revents = 0;
                break;
            
        }
        int ready = poll(pollfds, 1, 1000);

        if (ready < 0)
        {
            perror("poll");
            break;
        }

        if (ready == 0)
        {
            // Time out
            printf("Poll timeout\n");
            continue;
        }

        // Handle the state of sock fd after poll returned.
        if (pollfds[0].fd > 0)
        {
            if (pollfds[0].revents == POLLERR || pollfds[0].revents == POLLHUP || pollfds[0].revents == POLLNVAL)
            {
                conn_state = CONN_ERROR;
                fprintf(stderr, "Error while communicating.\n");
            }
            else
            {
                switch (conn_state)
                {
                    case CONN_CONNECTING:
                        if (pollfds[0].revents & POLLOUT)
                        {
                            // Check if connection succeeded.
                            int error = 0;
                            socklen_t len = sizeof(error);
                            if (getsockopt(pollfds[0].fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                            {
                                printf("Connection established\n");
                                conn_state = CONN_SENDING;
                            }
                            else
                            {
                                printf("Connection failed: %s\n", strerror(error));
                                conn_state = CONN_ERROR;
                            }
                        }
                        break;
                    case CONN_SENDING:
                        if (pollfds[0].revents & POLLOUT)
                        {
                            int remaining = strlen(request) - bytes_sent;
                            if (remaining <= 0)
                            {
                                printf("Request sent, waiting for response.\n");
                                conn_state = CONN_RECEIVING;
                            }
                            else
                            {
                                int sent = send(pollfds[0].fd, request + bytes_sent, remaining, 0);
                                if (sent > 0)
                                {
                                    bytes_sent += sent;
                                    printf("%d bytes has been sent.\n", bytes_sent);
                                    if (bytes_sent == strlen(request))
                                    {
                                        printf("Request sent, waiting for response.\n");
                                        conn_state = CONN_RECEIVING;
                                    }
                                }
                                else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                                {
                                    perror("Send");
                                    conn_state = CONN_ERROR;
                                }
                            }
                        }
                        break;
                    case CONN_RECEIVING:
                        if (pollfds[0].revents & POLLIN)
                        {
                            int remaining = sizeof(response) - bytes_received;
                            if (remaining <= 0)
                            {
                                printf("Response received.\n[%s]\n", response);
                                conn_state = CONN_COMPLETED;
                            }
                            else
                            {
                                int received = recv(pollfds[0].fd, response + bytes_received, remaining, 0);
                                if (received > 0)
                                {
                                    bytes_received += received;
                                    printf("%d bytes has been received.\n", bytes_received);
                                    if (bytes_received == sizeof(response))
                                    {
                                        printf("Response received.\n[%s]\n", response);
                                        conn_state = CONN_COMPLETED;
                                    }
                                    else{
                                        response[bytes_received] = '\0';
                                        if (strstr(response, "\r\n\r\n") )
                                        {
                                            // Found Header completed line, stop receiving.
                                            printf("Response received.\n[%s]\n", response);
                                            conn_state = CONN_COMPLETED;
                                        }
                                    }
                                }
                                else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                                {
                                    perror("Receive.");
                                    conn_state = CONN_ERROR;
                                }
                            }
                        }
                        break;
                }
            }
        }
    }
    if (pollfds[0].fd > 0)
    {
        close(pollfds[0].fd);
    }

    printf("Accessing %s with poll is done.\n ", host);

    return EXIT_SUCCESS;

}