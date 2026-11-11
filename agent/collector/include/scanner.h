
#ifndef SCANNER_H
#define SCANNER_H

#include <stddef.h>

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
} DirectoryResult;

// Lire les entrées directes d'un répertoire
DirectoryResult *list_directory(const char *path);

// Libérer le résultat
void free_directory_result(DirectoryResult *result);

// Convertir le résultat en JSON
char *directory_result_to_json(const DirectoryResult *result);

#endif
