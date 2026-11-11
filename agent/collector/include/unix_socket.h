
#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#define SOCKET_PATH "/tmp/filewatch-collector.sock"
#define MAX_MESSAGE_SIZE (1024 * 1024)

int unix_server_start(const char *socket_path);

#endif
