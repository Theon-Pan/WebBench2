#include "bench_poll.h"
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

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