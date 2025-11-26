#include "communicator.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>


static int Socket(const char *host, const int port)
{
    int sockfd;
    struct addrinfo hints = {
        .ai_family = AF_INET,            // IPv4
        .ai_socktype = SOCK_STREAM,      // TCP
        .ai_flags = 0,
        .ai_protocol = 0
    };
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
            continue;   // Try next address.
        }

        // Connect to check if the sockfd is available. If not, close the sockfd and try the next.
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == -1)
        {
            close(sockfd);
            continue;   // Try next address.
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

int communicator(const Arguments *args, const HTTPRequest *http_request)
{
    return 0;
}