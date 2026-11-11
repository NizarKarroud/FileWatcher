
#include <stdio.h>
#include <stdlib.h>

#include "unix_socket.h"

int main(void)
{
    printf("[FileWatch] Starting collector...\n");

    if (unix_server_start(SOCKET_PATH) == -1) {
        fprintf(stderr, "[FileWatch] Server stopped\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
