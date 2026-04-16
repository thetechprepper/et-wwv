#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// POSIX / GNU
#include <getopt.h>
#include <unistd.h>

enum { RIFF_CHUNK_HEADER_SIZE = 8 };

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -f <file> [-d]\n", prog);
}

// Reads a little-endian 16-bit value; requires at least 2 bytes
static uint16_t read_le16(const unsigned char *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

// Reads a little-endian 32-bit value; requires at least 4 bytes
static uint32_t read_le32(const unsigned char *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;
    unsigned char header[12];
    unsigned char chunk_header[RIFF_CHUNK_HEADER_SIZE];
    unsigned char fmt_data[16];
    int debug = 0;
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
        {"debug", no_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:hd", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                file_path = optarg;
                break;
            case 'd':
                debug = 1;
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
        fprintf(stderr, "Error opening file '%s': %s\n",
            file_path, strerror(errno));
        return 1;
    }

    if (debug) {
        printf("Debug: opened file: %s\n", file_path);
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "Error: could not read WAV header: %s\n", file_path);
        fclose(fp);
        return 1;
    }

    // Verify RIFF container and WAVE type (first 12 bytes only)
    // This is a lightweight sanity check, not a full WAV parser.
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "Error: not a valid WAV file: %s\n", file_path);
        fclose(fp);
        return 1;
    }

    if (debug) {
        printf("Debug: valid RIFF/WAVE header detected\n");
    }

    // Main WAV parsing loop: read and process RIFF chunks one at a time
    while (fread(chunk_header, 1, sizeof(chunk_header), fp) == sizeof(chunk_header)) {
        uint32_t chunk_size = read_le32(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                fprintf(stderr, "Error: invalid fmt chunk: %s\n", file_path);
                fclose(fp);
                return 1;
            }

            if (fread(fmt_data, 1, sizeof(fmt_data), fp) != sizeof(fmt_data)) {
                fprintf(stderr, "Error: could not read fmt chunk: %s\n", file_path);
                fclose(fp);
                return 1;
            }

            audio_format = read_le16(fmt_data + 0);
            num_channels = read_le16(fmt_data + 2);
            sample_rate = read_le32(fmt_data + 4);
            bits_per_sample = read_le16(fmt_data + 14);
            found_fmt = 1;

            if (debug) {
                printf("Debug: fmt chunk found\n");
                printf("Debug: audio_format=%u channels=%u sample_rate=%u bits_per_sample=%u\n",
                       audio_format, num_channels, sample_rate, bits_per_sample);
            }

            if (chunk_size > 16) {
                if (fseek(fp, chunk_size - 16, SEEK_CUR) != 0) {
                    fprintf(stderr, "Error: could not skip fmt extension: %s\n", file_path);
                    fclose(fp);
                    return 1;
                }
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            data_chunk_size = chunk_size;
            data_chunk_offset = ftell(fp);
            found_data = 1;

            if (debug) {
                printf("Debug: data chunk found\n");
                printf("Debug: data_chunk_size=%u data_chunk_offset=%ld\n",
                       data_chunk_size, data_chunk_offset);
            }
            break;
        } else {
            if (fseek(fp, chunk_size, SEEK_CUR) != 0) {
                fprintf(stderr, "Error: could not skip chunk: %s\n", file_path);
                fclose(fp);
                return 1;
            }
        }

        if (chunk_size & 1) {
            if (fseek(fp, 1, SEEK_CUR) != 0) {
                fprintf(stderr, "Error: could not skip chunk padding: %s\n", file_path);
                fclose(fp);
                return 1;
            }
        }
    }

    if (!found_fmt) {
        fprintf(stderr, "Error: fmt chunk not found: %s\n", file_path);
        fclose(fp);
        return 1;
    }

    if (!found_data) {
        fprintf(stderr, "Error: data chunk not found: %s\n", file_path);
        fclose(fp);
        return 1;
    }

    if (audio_format != 1) {
        fprintf(stderr, "Error: unsupported WAV format: %s\n", file_path);
        fclose(fp);
        return 1;
    }

    if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 32) {
        fprintf(stderr, "Error: unsupported bits per sample: %u\n", bits_per_sample);
        fclose(fp);
        return 1;
    }

    if (num_channels == 0 || sample_rate == 0 || bits_per_sample == 0) {
        fprintf(stderr, "Error: invalid WAV format values: %s\n", file_path);
        fclose(fp);
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
        const double target_freq = 1000.0;
        const uint32_t window_ms = 10;
        const double expected_tone_duration = 0.8;
        const double min_match_duration = 0.6;
        const double max_match_duration = 1.0;
        uint32_t window_samples = (sample_rate * window_ms) / 1000;
        double k;
        double omega;
        double coeff;
        double q0 = 0.0;
        double q1 = 0.0;
        double q2 = 0.0;
        uint32_t window_index = 0;
        uint32_t window_count = 0;
        uint32_t estimated_window_count = 0;
        double min_power = -1.0;
        double max_power = -1.0;
        double sum_power = 0.0;
        double threshold = 0.0;
        double *window_powers = NULL;
        int *tone_present = NULL;

        if (frame_size == 0) {
            fprintf(stderr, "Error: invalid frame size\n");
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        if (window_samples == 0) {
            window_samples = 1;
        }

        total_frames = data_chunk_size / frame_size;
        total_samples = total_frames * num_channels;
        duration_seconds = (double)total_frames / (double)sample_rate;

        estimated_window_count = (uint32_t)(total_frames / window_samples);
        if (estimated_window_count == 0) {
            estimated_window_count = 1;
        }

        window_powers = malloc(sizeof(double) * estimated_window_count);
        if (window_powers == NULL) {
            fprintf(stderr, "Error: memory allocation failed\n");
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        tone_present = malloc(sizeof(int) * estimated_window_count);
        if (tone_present == NULL) {
            fprintf(stderr, "Error: memory allocation failed\n");
            free(window_powers);
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        k = 0.5 + ((double)window_samples * target_freq / (double)sample_rate);
        omega = (2.0 * M_PI * k) / (double)window_samples;
        coeff = 2.0 * cos(omega);

        if (debug) {
            printf("Debug: beginning PCM sample read\n");
            printf("Debug: bytes_per_sample=%u frame_size=%u total_frames=%llu total_samples=%llu\n",
                   bytes_per_sample,
                   frame_size,
                   (unsigned long long)total_frames,
                   (unsigned long long)total_samples);
            printf("Debug: window_ms=%u window_samples=%u target_freq=%.1f\n",
                   window_ms,
                   window_samples,
                   target_freq);
        }

        if (fseek(fp, data_chunk_offset, SEEK_SET) != 0) {
            fprintf(stderr, "Error: could not seek to data chunk\n");
            free(tone_present);
            free(window_powers);
            fclose(fp);
            usage(argv[0]);
            return 1;
        }

        for (uint64_t frame = 0; frame < total_frames; frame++) {
            float mono_sample = 0.0f;

            for (uint16_t channel = 0; channel < num_channels; channel++) {
                float sample_value = 0.0f;

                if (fread(sample_buf, 1, bytes_per_sample, fp) != bytes_per_sample) {
                    fprintf(stderr, "Error: could not read PCM sample data\n");
                    free(tone_present);
                    free(window_powers);
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

                mono_sample += sample_value;
            }

            mono_sample /= (float)num_channels;

            q0 = coeff * q1 - q2 + mono_sample;
            q2 = q1;
            q1 = q0;
            window_index++;

            if (window_index == window_samples) {
                double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;

                if (window_count < estimated_window_count) {
                    window_powers[window_count] = power;
                }

                if (min_power < 0.0 || power < min_power) {
                    min_power = power;
                }

                if (max_power < 0.0 || power > max_power) {
                    max_power = power;
                }

                sum_power += power;
                window_count++;

                q0 = 0.0;
                q1 = 0.0;
                q2 = 0.0;
                window_index = 0;
            }
        }

        threshold = window_count > 0 ? (sum_power / (double)window_count) : 0.0;

        if (debug) {
            printf("Debug: PCM sample read complete\n");
            printf("Stats: duration_seconds=%.3f\n", duration_seconds);
            printf("Stats: min_sample=%.6f max_sample=%.6f\n", min_sample, max_sample);
            printf("Stats: window_count=%u min_power=%.6f max_power=%.6f avg_power=%.6f threshold=%.6f\n",
                   window_count,
                   min_power,
                   max_power,
                   window_count > 0 ? sum_power / (double)window_count : 0.0,
                   threshold);
        }

        for (uint32_t i = 0; i < window_count && i < estimated_window_count; i++) {
            double window_start = ((double)i * (double)window_samples) / (double)sample_rate;
            double window_end = ((double)(i + 1) * (double)window_samples) / (double)sample_rate;

            tone_present[i] = window_powers[i] >= threshold;

            if (debug) {
                printf("Debug: window=%u start=%.3f end=%.3f power=%.6f tone=%s\n",
                       i,
                       window_start,
                       window_end,
                       window_powers[i],
                       tone_present[i] ? "present" : "absent");
            }
        }

        {
            int in_interval = 0;
            uint32_t interval_start_index = 0;
            uint32_t interval_count = 0;
            uint32_t candidate_count = 0;

            printf("WWV 1000 Hz candidate tones:\n");

            for (uint32_t i = 0; i < window_count && i < estimated_window_count; i++) {
                if (!in_interval && tone_present[i]) {
                    in_interval = 1;
                    interval_start_index = i;
                } else if (in_interval && !tone_present[i]) {
                    double start_time = ((double)interval_start_index * (double)window_samples) / (double)sample_rate;
                    double end_time = ((double)i * (double)window_samples) / (double)sample_rate;
                    double interval_duration = end_time - start_time;

                    if (debug) {
                        printf("Interval: start=%.3f end=%.3f duration=%.3f\n",
                               start_time,
                               end_time,
                               interval_duration);
                    }

                    if (interval_duration >= min_match_duration && interval_duration <= max_match_duration) {
                        candidate_count++;
                        printf("%u. start=%.3f end=%.3f duration=%.3f expected=%.3f\n",
                               candidate_count,
                               start_time,
                               end_time,
                               interval_duration,
                               expected_tone_duration);
                    }

                    interval_count++;
                    in_interval = 0;
                }
            }

            if (in_interval) {
                double start_time = ((double)interval_start_index * (double)window_samples) / (double)sample_rate;
                double end_time = ((double)window_count * (double)window_samples) / (double)sample_rate;
                double interval_duration = end_time - start_time;

                if (debug) {
                    printf("Interval: start=%.3f end=%.3f duration=%.3f\n",
                           start_time,
                           end_time,
                           interval_duration);
                }

                if (interval_duration >= min_match_duration && interval_duration <= max_match_duration) {
                    candidate_count++;
                    printf("%u. start=%.3f end=%.3f duration=%.3f expected=%.3f\n",
                           candidate_count,
                           start_time,
                           end_time,
                           interval_duration,
                           expected_tone_duration);
                }

                interval_count++;
            }

            if (candidate_count == 0) {
                printf("None\n");
            }

            printf("Stats: interval_count=%u candidate_count=%u\n",
                   interval_count,
                   candidate_count);
        }

        free(tone_present);
        free(window_powers);
    }

    fclose(fp);
    return 0;
}
