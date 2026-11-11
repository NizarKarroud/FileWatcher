
#ifndef SCANNER_H
#define SCANNER_H

#include <stddef.h>

#define DEFAULT_PAGE_SIZE 10000
#define MAX_PAGE_SIZE 10000
#define PAGE_BYTE_BUDGET (768 * 1024)

typedef enum {
    FILE_TYPE,
    DIRECTORY_TYPE,
    SYMLINK_TYPE,
    OTHER_TYPE
} EntryType;

typedef struct {
    char *name;
    EntryType type;
} FileEntry;

typedef struct {
    char *path;
    FileEntry *entries;
    size_t count;

    int partial;
    int error_code;
    int has_more;
} DirectoryResult;

typedef struct DirectorySession DirectorySession;

DirectorySession *directory_session_open(const char *path);

DirectoryResult *directory_session_next(
    DirectorySession *session,
    size_t limit
);

void directory_session_close(DirectorySession *session);

void free_directory_result(DirectoryResult *result);

char *directory_result_to_json(const DirectoryResult *result);

#endif
