#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdint.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -f <file>\n", prog);
}

static uint16_t read_le16(const unsigned char *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static uint32_t read_le32(const unsigned char *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;
    unsigned char header[12];
    unsigned char chunk_header[8];
    unsigned char fmt_data[16];
    int found_fmt = 0;
    int found_data = 0;
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;

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

    while (fread(chunk_header, 1, sizeof(chunk_header), fp) == sizeof(chunk_header)) {
        uint32_t chunk_size = read_le32(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                fprintf(stderr, "Error: invalid fmt chunk: %s\n", file_path);
                fclose(fp);
                usage(argv[0]);
                return 1;
            }

            if (fread(fmt_data, 1, sizeof(fmt_data), fp) != sizeof(fmt_data)) {
                fprintf(stderr, "Error: could not read fmt chunk: %s\n", file_path);
                fclose(fp);
                usage(argv[0]);
                return 1;
            }

            audio_format = read_le16(fmt_data + 0);
            num_channels = read_le16(fmt_data + 2);
            sample_rate = read_le32(fmt_data + 4);
            bits_per_sample = read_le16(fmt_data + 14);
            found_fmt = 1;

            if (chunk_size > 16) {
                if (fseek(fp, chunk_size - 16, SEEK_CUR) != 0) {
                    fprintf(stderr, "Error: could not skip fmt extension: %s\n", file_path);
                    fclose(fp);
                    usage(argv[0]);
                    return 1;
                }
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            found_data = 1;
            break;
        } else {
            if (fseek(fp, chunk_size, SEEK_CUR) != 0) {
                fprintf(stderr, "Error: could not skip chunk: %s\n", file_path);
                fclose(fp);
                usage(argv[0]);
                return 1;
            }
        }

        if (chunk_size & 1) {
            if (fseek(fp, 1, SEEK_CUR) != 0) {
                fprintf(stderr, "Error: could not skip chunk padding: %s\n", file_path);
                fclose(fp);
                usage(argv[0]);
                return 1;
            }
        }
    }

    if (!found_fmt) {
        fprintf(stderr, "Error: fmt chunk not found: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    if (!found_data) {
        fprintf(stderr, "Error: data chunk not found: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    if (audio_format != 1) {
        fprintf(stderr, "Error: unsupported WAV format: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    if (num_channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
        fprintf(stderr, "Error: invalid WAV format values: %s\n", file_path);
        fclose(fp);
        usage(argv[0]);
        return 1;
    }

    fclose(fp);
    return 0;
}