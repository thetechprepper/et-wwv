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
    uint32_t data_chunk_size = 0;
    long data_chunk_offset = 0;

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

    printf("Debug: opened file: %s\n", file_path);

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

    printf("Debug: valid RIFF/WAVE header detected\n");

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

            printf("Debug: fmt chunk found\n");
            printf("Debug: audio_format=%u channels=%u sample_rate=%u bits_per_sample=%u\n",
                   audio_format, num_channels, sample_rate, bits_per_sample);

            if (chunk_size > 16) {
                if (fseek(fp, chunk_size - 16, SEEK_CUR) != 0) {
                    fprintf(stderr, "Error: could not skip fmt extension: %s\n", file_path);
                    fclose(fp);
                    usage(argv[0]);
                    return 1;
                }
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_chunk_size = chunk_size;
            data_chunk_offset = ftell(fp);
            found_data = 1;

            printf("Debug: data chunk found\n");
            printf("Debug: data_chunk_size=%u data_chunk_offset=%ld\n",
                   data_chunk_size, data_chunk_offset);
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

    if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 32) {
        fprintf(stderr, "Error: unsupported bits per sample: %u\n", bits_per_sample);
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

    {
        uint32_t bytes_per_sample = bits_per_sample / 8;
        uint32_t frame_size = num_channels * bytes_per_sample;
        uint64_t total_frames = 0;
        uint64_t total_samples = 0;
        double duration_seconds = 0.0;
        float min_sample = 1.0f;
        float max_sample = -1.0f;
        unsigned char sample_buf[4];

        if (frame_size == 0) {
            fprintf(stderr, "Error: invalid frame size\n");
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        total_frames = data_chunk_size / frame_size;
        total_samples = total_frames * num_channels;
        duration_seconds = (double)total_frames / (double)sample_rate;

        printf("Debug: beginning PCM sample read\n");
        printf("Debug: bytes_per_sample=%u frame_size=%u total_frames=%llu total_samples=%llu\n",
               bytes_per_sample,
               frame_size,
               (unsigned long long)total_frames,
               (unsigned long long)total_samples);

        if (fseek(fp, data_chunk_offset, SEEK_SET) != 0) {
            fprintf(stderr, "Error: could not seek to data chunk\n");
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        for (uint64_t frame = 0; frame < total_frames; frame++) {
            for (uint16_t channel = 0; channel < num_channels; channel++) {
                float sample_value = 0.0f;

                if (fread(sample_buf, 1, bytes_per_sample, fp) != bytes_per_sample) {
                    fprintf(stderr, "Error: could not read PCM sample data\n");
                    fclose(fp);
                    usage(argv[0]);
                    return 1;
                }

                if (bits_per_sample == 8) {
                    uint8_t s = sample_buf[0];
                    sample_value = ((float)s - 128.0f) / 128.0f;
                } else if (bits_per_sample == 16) {
                    int16_t s = (int16_t)read_le16(sample_buf);
                    sample_value = (float)s / 32768.0f;
                } else if (bits_per_sample == 32) {
                    int32_t s = (int32_t)read_le32(sample_buf);
                    sample_value = (float)s / 2147483648.0f;
                }

                if (sample_value < min_sample) {
                    min_sample = sample_value;
                }

                if (sample_value > max_sample) {
                    max_sample = sample_value;
                }
            }
        }

        printf("Debug: PCM sample read complete\n");
        printf("Stats: duration_seconds=%.3f\n", duration_seconds);
        printf("Stats: min_sample=%.6f max_sample=%.6f\n", min_sample, max_sample);
    }

    fclose(fp);
    return 0;
}