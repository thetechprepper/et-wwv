#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

int main(int argc, char *argv[]) {
    const char *file_path = NULL;

    static struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                file_path = optarg;
                break;
            default:
                return 1;
        }
    }

    if (file_path == NULL) {
        return 1;
    }

    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        return 1;
    }

    fclose(fp);
    return 0;
}