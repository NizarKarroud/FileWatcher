
#define _POSIX_C_SOURCE 200809L

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <cjson/cJSON.h>

struct DirectorySession {
    DIR *dir;
    char *path;

    char *pending_name;
    EntryType pending_type;
};


static EntryType get_entry_type(mode_t mode)
{
    if (S_ISREG(mode))
        return FILE_TYPE;

    if (S_ISDIR(mode))
        return DIRECTORY_TYPE;

    if (S_ISLNK(mode))
        return SYMLINK_TYPE;

    return OTHER_TYPE;
}


DirectorySession *directory_session_open(const char *path)
{
    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    DIR *dir = opendir(path);

    if (dir == NULL)
        return NULL;

    DirectorySession *session = calloc(1, sizeof(*session));

    if (session == NULL) {
        int saved_errno = errno;
        closedir(dir);
        errno = saved_errno;
        return NULL;
    }

    session->path = strdup(path);

    if (session->path == NULL) {
        int saved_errno = errno;
        closedir(dir);
        free(session);
        errno = saved_errno;
        return NULL;
    }

    session->dir = dir;
    return session;
}


static int read_next_entry(
    DirectorySession *session,
    DirectoryResult *result,
    FileEntry *output
)
{
    struct dirent *entry;

    while (1) {
        errno = 0;
        entry = readdir(session->dir);

        if (entry == NULL) {
            if (errno != 0)
                return -1;

            return 0;
        }

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        struct stat st;


        if (fstatat(
                dirfd(session->dir),
                entry->d_name,
                &st,
                AT_SYMLINK_NOFOLLOW
            ) == -1) {

            result->partial = 1;

            if (result->error_code == 0)
                result->error_code = errno;

            continue;
        }

        output->name = strdup(entry->d_name);

        if (output->name == NULL)
            return -1;

        output->type = get_entry_type(st.st_mode);

        return 1;
    }
}



static size_t entry_budget(const char *name)
{
    return strlen(name) * 6 + 128;
}


DirectoryResult *directory_session_next(
    DirectorySession *session,
    size_t limit
)
{
    if (session == NULL || limit == 0 ||
        limit > MAX_PAGE_SIZE) {
        errno = EINVAL;
        return NULL;
    }

    DirectoryResult *result = calloc(1, sizeof(*result));

    if (result == NULL)
        return NULL;

    result->path = strdup(session->path);

    if (result->path == NULL)
        goto error;

    size_t capacity = 0;
    size_t budget_used = 4096;

    while (result->count < limit) {
        FileEntry next = {0};

        if (session->pending_name != NULL) {
            next.name = session->pending_name;
            next.type = session->pending_type;
            session->pending_name = NULL;
        } else {
            int status = read_next_entry(
                session, result, &next
            );

            if (status < 0)
                goto error;

            if (status == 0) {
                result->has_more = 0;
                return result;
            }
        }

        size_t cost = entry_budget(next.name);

        if (budget_used + cost > PAGE_BYTE_BUDGET) {
            session->pending_name = next.name;
            session->pending_type = next.type;
            result->has_more = 1;
            return result;
        }

        if (result->count == capacity) {
            size_t new_capacity =
                capacity == 0 ? 64 : capacity * 2;

            if (new_capacity > limit)
                new_capacity = limit;

            if (new_capacity > SIZE_MAX / sizeof(FileEntry)) {
                free(next.name);
                errno = ENOMEM;
                goto error;
            }

            FileEntry *tmp = realloc(
                result->entries,
                new_capacity * sizeof(FileEntry)
            );

            if (tmp == NULL) {
                int saved_errno = errno;
                free(next.name);
                errno = saved_errno;
                goto error;
            }

            result->entries = tmp;
            capacity = new_capacity;
        }

        result->entries[result->count] = next;
        result->count++;
        budget_used += cost;
    }

    FileEntry next = {0};

    int status = read_next_entry(session, result, &next);

    if (status < 0)
        goto error;

    if (status == 1) {
        session->pending_name = next.name;
        session->pending_type = next.type;
        result->has_more = 1;
    } else {
        result->has_more = 0;
    }

    return result;

error:
    {
        int saved_errno = errno;
        free_directory_result(result);
        errno = saved_errno;
        return NULL;
    }
}


void directory_session_close(DirectorySession *session)
{
    if (session == NULL)
        return;

    if (session->dir != NULL)
        closedir(session->dir);

    free(session->pending_name);
    free(session->path);
    free(session);
}


void free_directory_result(DirectoryResult *result)
{
    if (result == NULL)
        return;

    for (size_t i = 0; i < result->count; i++)
        free(result->entries[i].name);

    free(result->entries);
    free(result->path);
    free(result);
}


char *directory_result_to_json(const DirectoryResult *result)
{
    if (result == NULL) {
        errno = EINVAL;
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();

    if (root == NULL)
        return NULL;

    if (!cJSON_AddStringToObject(
            root, "type", "directory_listing") ||
        !cJSON_AddStringToObject(
            root, "path", result->path) ||
        !cJSON_AddBoolToObject(
            root, "partial", result->partial) ||
        !cJSON_AddNumberToObject(
            root, "error_code", result->error_code) ||
        !cJSON_AddBoolToObject(
            root, "has_more", result->has_more) ||
        !cJSON_AddNumberToObject(
            root, "count", (double)result->count)) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");

    if (entries == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < result->count; i++) {
        const FileEntry *file = &result->entries[i];

        const char *type = "other";

        switch (file->type) {
            case FILE_TYPE:
                type = "file";
                break;

            case DIRECTORY_TYPE:
                type = "directory";
                break;

            case SYMLINK_TYPE:
                type = "symlink";
                break;

            default:
                break;
        }

        cJSON *item = cJSON_CreateObject();

        if (item == NULL ||
            !cJSON_AddStringToObject(
                item, "name", file->name) ||
            !cJSON_AddStringToObject(
                item, "type", type)) {

            cJSON_Delete(item);
            cJSON_Delete(root);
            return NULL;
        }

        cJSON_AddItemToArray(entries, item);
    }

    char *json = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (json == NULL)
        errno = ENOMEM;

    return json;
}
