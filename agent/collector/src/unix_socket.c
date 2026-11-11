
#define _GNU_SOURCE

#include "unix_socket.h"
#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/random.h>
#include <arpa/inet.h>

#include <cjson/cJSON.h>


typedef struct {
    char cursor[33];
    DirectorySession *session;
    char *path;
    time_t last_access;
    int active;
} SessionSlot;

static SessionSlot sessions[MAX_SESSIONS];


static int read_exact(int fd, void *buffer, size_t size)
{
    size_t received = 0;

    while (received < size) {
        ssize_t n = recv(
            fd, (char *)buffer + received,
            size - received, 0
        );

        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        received += (size_t)n;
    }

    return 0;
}


static int write_exact(int fd, const void *buffer, size_t size)
{
    size_t sent = 0;

    while (sent < size) {
        ssize_t n = send(
            fd, (const char *)buffer + sent,
            size - sent, MSG_NOSIGNAL
        );

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}


static char *receive_message(int fd)
{
    uint32_t net_length;

    if (read_exact(fd, &net_length, 4) < 0)
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


static int send_message(int fd, const char *json)
{
    size_t length = strlen(json);

    if (length == 0 || length > MAX_MESSAGE_SIZE) {
        errno = EMSGSIZE;
        return -1;
    }

    uint32_t net_length = htonl((uint32_t)length);

    if (write_exact(fd, &net_length, 4) < 0)
        return -1;

    return write_exact(fd, json, length);
}


static void close_slot(SessionSlot *slot)
{
    if (!slot->active)
        return;

    directory_session_close(slot->session);
    free(slot->path);

    memset(slot, 0, sizeof(*slot));
}


static void cleanup_sessions(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        SessionSlot *slot = &sessions[i];

        if (slot->active &&
            difftime(now, slot->last_access) >= SESSION_TIMEOUT) {
            close_slot(slot);
        }
    }
}


static int generate_cursor(char output[33])
{
    unsigned char random_bytes[16];
    size_t received = 0;

    while (received < sizeof(random_bytes)) {
        ssize_t n = getrandom(
            random_bytes + received,
            sizeof(random_bytes) - received,
            0
        );

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        received += (size_t)n;
    }

    const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 16; i++) {
        output[i * 2] = hex[random_bytes[i] >> 4];
        output[i * 2 + 1] = hex[random_bytes[i] & 15];
    }

    output[32] = '\0';
    return 0;
}


static SessionSlot *find_session(const char *cursor)
{
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active &&
            strcmp(sessions[i].cursor, cursor) == 0) {
            return &sessions[i];
        }
    }

    return NULL;
}


static SessionSlot *create_session(const char *path)
{
    SessionSlot *slot = NULL;

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            slot = &sessions[i];
            break;
        }
    }

    if (slot == NULL) {
        errno = EMFILE;
        return NULL;
    }

    DirectorySession *session = directory_session_open(path);

    if (session == NULL)
        return NULL;

    char *copy = strdup(path);

    if (copy == NULL) {
        directory_session_close(session);
        return NULL;
    }

    char cursor[33];

    do {
        if (generate_cursor(cursor) < 0) {
            int saved_errno = errno;
            free(copy);
            directory_session_close(session);
            errno = saved_errno;
            return NULL;
        }
    } while (find_session(cursor) != NULL);

    slot->session = session;
    slot->path = copy;
    slot->last_access = time(NULL);
    slot->active = 1;

    strcpy(slot->cursor, cursor);

    return slot;
}


static cJSON *make_error(
    const char *request_id,
    const char *code,
    const char *message
)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
        return NULL;

    cJSON *error = cJSON_CreateObject();

    if (error == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    if (!cJSON_AddStringToObject(root, "request_id", request_id) ||
        !cJSON_AddStringToObject(root, "status", "error") ||
        !cJSON_AddStringToObject(error, "code", code) ||
        !cJSON_AddStringToObject(error, "message", message)) {
        cJSON_Delete(error);
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddItemToObject(root, "error", error);

    return root;
}


static cJSON *process_request(cJSON *request)
{
    cJSON *id = cJSON_GetObjectItemCaseSensitive(
        request, "request_id"
    );

    cJSON *command = cJSON_GetObjectItemCaseSensitive(
        request, "command"
    );

    cJSON *path = cJSON_GetObjectItemCaseSensitive(
        request, "path"
    );

    cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(
        request, "limit"
    );

    cJSON *cursor_item = cJSON_GetObjectItemCaseSensitive(
        request, "cursor"
    );

    const char *request_id = "";

    if (cJSON_IsString(id) && id->valuestring != NULL &&
        strlen(id->valuestring) <= 128) {
        request_id = id->valuestring;
    } else {
        return make_error("", "INVALID_REQUEST", "Invalid request_id");
    }

    if (!cJSON_IsString(command) ||
        !cJSON_IsString(path) ||
        path->valuestring == NULL ||
        path->valuestring[0] != '/' ||
        strlen(path->valuestring) > 4096) {
        return make_error(
            request_id, "INVALID_REQUEST", "Invalid command or path"
        );
    }

    if (strcmp(command->valuestring, "LIST_DIRECTORY") != 0) {
        return make_error(
            request_id, "UNKNOWN_COMMAND", "Unknown command"
        );
    }

    size_t limit = DEFAULT_PAGE_SIZE;

    if (limit_item != NULL && !cJSON_IsNull(limit_item)) {
        if (!cJSON_IsNumber(limit_item) ||
            limit_item->valuedouble < 1 ||
            limit_item->valuedouble > MAX_PAGE_SIZE ||
            (double)(size_t)limit_item->valuedouble !=
                limit_item->valuedouble) {
            return make_error(
                request_id, "INVALID_LIMIT", "Invalid page limit"
            );
        }

        limit = (size_t)limit_item->valuedouble;
    }

    if (cursor_item != NULL &&
        !cJSON_IsNull(cursor_item) &&
        !cJSON_IsString(cursor_item)) {
        return make_error(
            request_id, "INVALID_CURSOR", "Invalid cursor format"
        );
    }

    const char *cursor = NULL;

    if (cJSON_IsString(cursor_item))
        cursor = cursor_item->valuestring;

    SessionSlot *slot;

    if (cursor == NULL) {
        slot = create_session(path->valuestring);

        if (slot == NULL) {
            int error = errno;
            return make_error(
                request_id, "SCAN_ERROR", strerror(error)
            );
        }
    } else {
        slot = find_session(cursor);

        if (slot == NULL) {
            return make_error(
                request_id, "CURSOR_EXPIRED",
                "Cursor not found or expired"
            );
        }

        if (strcmp(slot->path, path->valuestring) != 0) {
            return make_error(
                request_id, "INVALID_CURSOR",
                "Cursor belongs to a different directory"
            );
        }
    }

    DirectoryResult *result = directory_session_next(
        slot->session, limit
    );

    if (result == NULL) {
        int error = errno;
        close_slot(slot);
        return make_error(
            request_id, "SCAN_ERROR", strerror(error)
        );
    }

    int has_more = result->has_more;

    char *listing_json = directory_result_to_json(result);
    free_directory_result(result);

    if (listing_json == NULL) {
        close_slot(slot);
        return make_error(
            request_id, "SERIALIZATION_ERROR",
            "Unable to serialize directory listing"
        );
    }

    cJSON *data = cJSON_Parse(listing_json);
    cJSON_free(listing_json);

    if (data == NULL) {
        close_slot(slot);
        return make_error(
            request_id, "SERIALIZATION_ERROR",
            "Invalid directory JSON"
        );
    }

    cJSON *response = cJSON_CreateObject();

    if (response == NULL) {
        cJSON_Delete(data);
        close_slot(slot);
        return NULL;
    }

    cJSON *next_cursor = NULL;

    if (has_more) {
        next_cursor = cJSON_CreateString(slot->cursor);
    } else {
        next_cursor = cJSON_CreateNull();
    }

    if (next_cursor == NULL ||
        !cJSON_AddStringToObject(response, "request_id", request_id) ||
        !cJSON_AddStringToObject(response, "status", "ok")) {
        cJSON_Delete(next_cursor);
        cJSON_Delete(data);
        cJSON_Delete(response);
        close_slot(slot);
        return NULL;
    }

    cJSON_AddItemToObject(data, "next_cursor", next_cursor);
    cJSON_AddItemToObject(response, "data", data);

    if (has_more) {
        slot->last_access = time(NULL);
    } else {
        close_slot(slot);
    }

    return response;
}


static void handle_client(int client_fd)
{
    char *message = receive_message(client_fd);

    if (message == NULL) {
        perror("receive_message");
        return;
    }

    cJSON *request = cJSON_Parse(message);
    free(message);

    cJSON *response;

    if (request == NULL || !cJSON_IsObject(request)) {
        response = make_error(
            "", "INVALID_JSON", "Malformed JSON request"
        );
    } else {
        response = process_request(request);
    }

    cJSON_Delete(request);

    if (response == NULL) {
        response = make_error(
            "", "INTERNAL_ERROR", "Out of memory"
        );
    }

    if (response != NULL) {
        char *json = cJSON_PrintUnformatted(response);

        if (json != NULL) {
            if (send_message(client_fd, json) == -1)
                perror("send_message");

            cJSON_free(json);
        }

        cJSON_Delete(response);
    }
}


int unix_server_start(const char *socket_path)
{
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (server_fd == -1) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;

    if (socket_path == NULL ||
        strlen(socket_path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        close(server_fd);
        return -1;
    }

    strcpy(addr.sun_path, socket_path);

    unlink(socket_path);

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

    printf("[FileWatch] UNIX server running\n");
    printf("[FileWatch] Socket: %s\n", socket_path);
    fflush(stdout);

    while (1) {
        cleanup_sessions();

        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd == -1) {
            if (errno == EINTR)
                continue;

            perror("accept");
            break;
        }

        struct timeval timeout = {
            .tv_sec = 5,
            .tv_usec = 0
        };

        if (setsockopt(
                client_fd, SOL_SOCKET, SO_RCVTIMEO,
                &timeout, sizeof(timeout)) == -1 ||
            setsockopt(
                client_fd, SOL_SOCKET, SO_SNDTIMEO,
                &timeout, sizeof(timeout)) == -1) {
            perror("setsockopt");
            close(client_fd);
            continue;
        }

        handle_client(client_fd);
        close(client_fd);
    }

    for (int i = 0; i < MAX_SESSIONS; i++)
        close_slot(&sessions[i]);

    close(server_fd);
    unlink(socket_path);

    return -1;
}
