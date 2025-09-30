#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char* argv[]) {
    char* localhost = "127.0.0.1";

    char hostname[100] = {0};
    
    strncpy(hostname, localhost, 10);
    // hostname[5] = '\0';
    // snprintf(hostname, sizeof(hostname), "%s",localhost);

    printf("localhost: [%s] \n", localhost);
    printf("hostname: [%s] \n", hostname);
}