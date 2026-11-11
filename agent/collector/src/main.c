
#include <stdio.h>
#include <cjson/cJSON.h>

#include "scanner.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        return 1;
    }

    DirectoryResult *result = list_directory(argv[1]);

    if (result == NULL) {
        fprintf(stderr, "Directory scan failed\n");
        return 1;
    }

    char *json = directory_result_to_json(result);

    if (json != NULL) {
        printf("%s\n", json);
        cJSON_free(json);
    }

    free_directory_result(result);

    return json == NULL ? 1 : 0;
}
