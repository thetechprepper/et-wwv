#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -f <file>\n", prog);
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;

    static struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                file_path = optarg;
                break;
            case 'h':
                usage(argv[0]);
                return 1;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (file_path == NULL) {
        usage(argv[0]);
        return 1;
    }

    FILE *fp = fopen(file_path, "rb");
    if (fp == NULL) {
        usage(argv[0]);
        return 1;
    }

    fclose(fp);
    return 0;
}