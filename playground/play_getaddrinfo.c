#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = 0,
        .ai_protocol = 0
    };

    struct addrinfo *rp, *result;

    char *node = "www.google.com";
    char *service = "http";
    int ret = getaddrinfo(node, service, &hints, &result);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s.\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    rp = result;
    while (rp != NULL)
    {
        int sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (-1 == sockfd)
        {
            perror("Creating socket failed");
            rp = rp->ai_next;
            continue;
        }

        struct sockaddr_in *addr = (struct sockaddr_in *) rp->ai_addr;
        
        // Convert IP address to dotted decimal string.
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str));

        fprintf(stdout, "Connecting to %s:%d...\n", ip_str, ntohs(addr->sin_port));
        close(sockfd);
        rp = rp->ai_next;
    }

    return EXIT_SUCCESS;

}