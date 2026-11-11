
#define _POSIX_C_SOURCE 200809L

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <cjson/cJSON.h>



DirectoryResult *list_directory(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    DIR *dir = opendir(path);

    if (dir == NULL) {
        return NULL;  // errno défini par opendir()
    }

    DirectoryResult *result = calloc(1, sizeof(*result));

    if (result == NULL) {
        int saved_errno = errno;
        closedir(dir);
        errno = saved_errno;
        return NULL;
    }

    result->path = strdup(path);

    if (result->path == NULL)
        goto fatal_error;

    struct dirent *entry;

    while (1) {

        // Distinguer fin du répertoire et erreur de readdir()
        errno = 0;
        entry = readdir(dir);

        if (entry == NULL) {
            if (errno != 0)
                goto fatal_error;

            break;  // Fin normale
        }

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->d_name);
        int separator = (path[path_len - 1] != '/');

        if (path_len > SIZE_MAX - name_len - 2) {
            errno = ENAMETOOLONG;
            goto fatal_error;
        }

        char *full_path = malloc(
            path_len + separator + name_len + 1
        );

        if (full_path == NULL)
            goto fatal_error;

        sprintf(full_path, "%s%s%s",
                path,
                separator ? "/" : "",
                entry->d_name);

        struct stat st;

        if (lstat(full_path, &st) == -1) {

            // L'entrée a pu disparaître entre readdir et lstat
            result->partial = 1;

            if (result->error_code == 0)
                result->error_code = errno;

            free(full_path);
            continue;
        }

        free(full_path);

        EntryType type = OTHER_TYPE;

        if (S_ISREG(st.st_mode))
            type = FILE_TYPE;
        else if (S_ISDIR(st.st_mode))
            type = DIRECTORY_TYPE;
        else if (S_ISLNK(st.st_mode))
            type = SYMLINK_TYPE;

        if (result->count == SIZE_MAX / sizeof(FileEntry)) {
            errno = ENOMEM;
            goto fatal_error;
        }

        FileEntry *new_entries = realloc(
            result->entries,
            (result->count + 1) * sizeof(FileEntry)
        );

        if (new_entries == NULL)
            goto fatal_error;

        result->entries = new_entries;

        FileEntry *file = &result->entries[result->count];

        file->name = strdup(entry->d_name);

        if (file->name == NULL)
            goto fatal_error;

        file->type = type;
        result->count++;
    }

    if (closedir(dir) == -1) {
        int saved_errno = errno;
        free_directory_result(result);
        errno = saved_errno;
        return NULL;
    }

    return result;

fatal_error:
    {
        int saved_errno = errno;

        closedir(dir);
        free_directory_result(result);

        errno = saved_errno;
        return NULL;
    }
}


char *directory_result_to_json(const DirectoryResult *result)
{
    if (result == NULL)
        return NULL;

    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return NULL;

    if (!cJSON_AddStringToObject(root, "type", "directory_listing") ||
        !cJSON_AddStringToObject(root, "path", result->path)) {
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
            case FILE_TYPE:      type = "file"; break;
            case DIRECTORY_TYPE: type = "directory"; break;
            case SYMLINK_TYPE:   type = "symlink"; break;
            default:            break;
        }

        cJSON *item = cJSON_CreateObject();

        if (item == NULL ||
            !cJSON_AddStringToObject(item, "name", file->name) ||
            !cJSON_AddStringToObject(item, "type", type)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return NULL;
        }

        cJSON_AddItemToArray(entries, item);
    }

    char *json = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    return json;
}

void free_directory_result(DirectoryResult *result)
{
    if (result == NULL)
        return;

    for (size_t i = 0; i < result->count; i++) {
        free(result->entries[i].name);
    }

    free(result->entries);
    free(result->path);
    free(result);
}