
#define _POSIX_C_SOURCE 200809L

#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include <cjson/cJSON.h>


DirectoryResult *list_directory(const char *path)
{
    DIR *dir = opendir(path);

    if (dir == NULL) {
        perror("opendir");
        return NULL;
    }

    DirectoryResult *result = calloc(1, sizeof(DirectoryResult));

    if (result == NULL) {
        closedir(dir);
        return NULL;
    }

    result->path = strdup(path);

    if (result->path == NULL) {
        closedir(dir);
        free_directory_result(result);
        return NULL;
    }

    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t path_len = strlen(path);
        size_t name_len = strlen(entry->d_name);

        int separator = (path_len > 0 && path[path_len - 1] != '/');

        char *full_path = malloc(
            path_len + separator + name_len + 1
        );

        if (full_path == NULL)
            goto error;

        sprintf(full_path, "%s%s%s",
                path,
                separator ? "/" : "",
                entry->d_name);

        struct stat st;

        if (lstat(full_path, &st) == -1) {
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

        FileEntry *new_entries = realloc(
            result->entries,
            (result->count + 1) * sizeof(FileEntry)
        );

        if (new_entries == NULL)
            goto error;

        result->entries = new_entries;

        FileEntry *file = &result->entries[result->count];

        file->name = strdup(entry->d_name);

        if (file->name == NULL)
            goto error;

        file->type = type;
        result->count++;
    }

    closedir(dir);
    return result;

error:
    closedir(dir);
    free_directory_result(result);
    return NULL;
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
