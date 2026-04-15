#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -f <file>\n", prog);
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;
    unsigned char header[12];

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
        fprintf(stderr, "Error: file not found: %s\n", file_path);
        usage(argv[0]);
        return 1;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "Error: could not read WAV header: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Error: not a valid WAV file: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    fclose(fp);
    return 0;
}