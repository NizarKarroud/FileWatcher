
#include "unix_socket.h"
#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <cjson/cJSON.h>


// Lire exactement n octets
static int read_exact(int fd, void *buffer, size_t n)
{
    size_t received = 0;

    while (received < n) {
        ssize_t r = recv(
            fd,
            (char *)buffer + received,
            n - received,
            0
        );

        if (r == 0) {
            errno = ECONNRESET;
            return -1;
        }

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        received += (size_t)r;
    }

    return 0;
}


// Envoyer exactement n octets
static int write_exact(int fd, const void *buffer, size_t n)
{
    size_t sent = 0;

    while (sent < n) {
        ssize_t r = send(
            fd,
            (const char *)buffer + sent,
            n - sent,
            MSG_NOSIGNAL
        );

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (r == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)r;
    }

    return 0;
}


// Recevoir un message JSON
static char *receive_message(int fd)
{
    uint32_t net_length;

    if (read_exact(fd, &net_length, sizeof(net_length)) < 0)
        return NULL;

    uint32_t length = ntohl(net_length);

    if (length == 0 || length > MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return NULL;
    }

    char *buffer = malloc((size_t)length + 1);

    if (buffer == NULL)
        return NULL;

    if (read_exact(fd, buffer, length) < 0) {
        free(buffer);
        return NULL;
    }

    buffer[length] = '\0';

    return buffer;
}


// Envoyer un message JSON
static int send_message(int fd, const char *json)
{
    size_t length = strlen(json);

    if (length == 0 || length > MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t net_length = htonl((uint32_t)length);

    if (write_exact(fd, &net_length, sizeof(net_length)) < 0)
        return -1;

    return write_exact(fd, json, length);
}


// Construire une réponse d'erreur
static cJSON *make_error(const char *request_id,
                         const char *message)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
        return NULL;

    if (!cJSON_AddStringToObject(root, "request_id", request_id) ||
        !cJSON_AddStringToObject(root, "status", "error") ||
        !cJSON_AddStringToObject(root, "message", message)) {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}


// Traiter une requête du Worker Python
static void handle_client(int client_fd)
{
    char *request = receive_message(client_fd);

    if (request == NULL) {
        perror("receive_message");
        return;
    }

    cJSON *root = cJSON_Parse(request);
    free(request);

    cJSON *response = NULL;
    const char *request_id = "";

    if (root == NULL) {
        response = make_error("", "Invalid JSON");
        goto send_response;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");

    if (cJSON_IsString(id) && id->valuestring != NULL)
        request_id = id->valuestring;

    if (!cJSON_IsString(command) || !cJSON_IsString(path) ||
        !cJSON_IsString(id) || path->valuestring[0] != '/') {
        response = make_error(request_id, "Invalid request");
        goto send_response;
    }

    if (strcmp(command->valuestring, "LIST_DIRECTORY") != 0) {
        response = make_error(request_id, "Unknown command");
        goto send_response;
    }

    printf("[FileWatch] LIST_DIRECTORY %s\n", path->valuestring);

    DirectoryResult *result = list_directory(path->valuestring);

    if (result == NULL) {
        int scan_error = errno;
        response = make_error(request_id, strerror(scan_error));
        goto send_response;
    }

    char *json_data = directory_result_to_json(result);
    free_directory_result(result);

    if (json_data == NULL) {
        response = make_error(request_id, "Serialization failed");
        goto send_response;
    }

    cJSON *data = cJSON_Parse(json_data);
    cJSON_free(json_data);

    if (data == NULL) {
        response = make_error(request_id, "Invalid scanner response");
        goto send_response;
    }

    response = cJSON_CreateObject();

    if (response == NULL) {
        cJSON_Delete(data);
        goto send_response;
    }

    cJSON_AddStringToObject(response, "request_id", request_id);
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddItemToObject(response, "data", data);

send_response:
    if (response != NULL) {
        char *json = cJSON_PrintUnformatted(response);

        if (json != NULL) {
            if (send_message(client_fd, json) < 0)
                perror("send_message");

            cJSON_free(json);
        }

        cJSON_Delete(response);
    }

    cJSON_Delete(root);
}


// Démarrer le serveur UNIX
int unix_server_start(const char *socket_path)
{
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr = {0};

    addr.sun_family = AF_UNIX;

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long\n");
        close(server_fd);
        return -1;
    }

    strcpy(addr.sun_path, socket_path);

    // Supprimer l'ancien socket (prototype non-root uniquement)
    unlink(socket_path);

    // Restreindre l'accès au socket
    mode_t old_mask = umask(0077);

    int bind_result = bind(
        server_fd,
        (struct sockaddr *)&addr,
        sizeof(addr)
    );

    umask(old_mask);

    if (bind_result == -1) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        unlink(socket_path);
        return -1;
    }

    printf("[FileWatch] Socket: %s\n", socket_path);
    printf("[FileWatch] Waiting for Worker...\n");
    fflush(stdout);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            break;
        }

        printf("[FileWatch] Worker connected\n");
        fflush(stdout);

        // Limiter les attentes d'un client bloqué
        struct timeval timeout = {
            .tv_sec = 5,
            .tv_usec = 0
        };

        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout));

        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout));

        handle_client(client_fd);

        close(client_fd);
    }

    close(server_fd);
    unlink(socket_path);

    return -1;
}
