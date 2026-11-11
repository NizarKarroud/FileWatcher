
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <cjson/cJSON.h>

#include "scanner.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        return EXIT_FAILURE;
    }

    DirectoryResult *result = list_directory(argv[1]);

    if (result == NULL) {
        fprintf(stderr, "Scan failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    char *json = directory_result_to_json(result);

    if (json == NULL) {
        fprintf(stderr, "JSON serialization failed\n");
        free_directory_result(result);
        return EXIT_FAILURE;
    }

    printf("%s\n", json);

    cJSON_free(json);
    free_directory_result(result);

    return EXIT_SUCCESS;
}
