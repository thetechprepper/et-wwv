#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// POSIX / GNU
#include <getopt.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

enum {
    RIFF_CHUNK_HEADER_SIZE = 8,
    SAMPLE_BUFFER_FRAMES = 1024,
    DEFAULT_ALSA_CAPTURE_SECONDS = 120
};

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s (-f <file> | -a [--device <name>]) [-d] [--seconds <n>]\n", prog);
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

typedef int (*sample_reader_fn)(void *ctx,
                                float *mono_samples,
                                uint32_t sample_count,
                                float *min_sample,
                                float *max_sample);

struct wav_sample_reader {
    FILE *fp;
    uint16_t num_channels;
    uint16_t bits_per_sample;
};

struct alsa_sample_reader {
    snd_pcm_t *pcm;
    uint16_t num_channels;
    uint16_t bits_per_sample;
};

struct detector_state {
    uint16_t num_channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    int debug;

    uint32_t bytes_per_sample;
    uint32_t frame_size;
    uint64_t total_frames;
    uint64_t total_samples;
    double duration_seconds;

    float min_sample;
    float max_sample;

    double target_freq;
    uint32_t window_ms;
    double expected_tone_duration;
    double min_match_duration;
    double max_match_duration;
    uint32_t window_samples;

    double k;
    double omega;
    double coeff;
    double q0;
    double q1;
    double q2;

    uint32_t window_index;
    uint32_t window_count;
    uint32_t estimated_window_count;

    double min_power;
    double max_power;
    double sum_power;
    double threshold;

    double *window_powers;
    int *tone_present;

    int live_stop_enabled;
    int live_in_interval;
    uint32_t live_interval_start_index;
    uint32_t live_candidate_count;

    int set_clock_enabled;
    struct timespec target_time;
};

static void print_local_time(const char *label, time_t t) {
    struct tm tm_buf;
    char time_buf[64];

    if (localtime_r(&t, &tm_buf) == NULL) {
        fprintf(stderr, "Error: could not convert local time\n");
        return;
    }

    if (strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S %Z", &tm_buf) == 0) {
        fprintf(stderr, "Error: could not format local time\n");
        return;
    }

    printf("%s: %s\n", label, time_buf);
}

static int get_next_minute_time(struct timespec *ts) {
    time_t now;
    struct tm tm_buf;

    now = time(NULL);
    if (now == (time_t)-1) {
        fprintf(stderr, "Error: could not read current time\n");
        return 1;
    }

    if (localtime_r(&now, &tm_buf) == NULL) {
        fprintf(stderr, "Error: could not convert current time\n");
        return 1;
    }

    tm_buf.tm_sec = 0;
    tm_buf.tm_min += 1;

    ts->tv_sec = mktime(&tm_buf);
    ts->tv_nsec = 0;

    if (ts->tv_sec == (time_t)-1) {
        fprintf(stderr, "Error: could not calculate target time\n");
        return 1;
    }

    return 0;
}

static int set_system_time(const struct timespec *ts) {
    if (clock_settime(CLOCK_REALTIME, ts) != 0) {
        fprintf(stderr, "\033[31mError: could not set system clock: %s\033[0m\n",
            strerror(errno));

        if (errno == EPERM) {
            fprintf(stderr, "\033[33mHint: re-run with sudo\033[0m\n");
        }

        return 1;
    }

    return 0;
}

static int read_wav_mono_samples(void *ctx,
                                 float *mono_samples,
                                 uint32_t sample_count,
                                 float *min_sample,
                                 float *max_sample) {
    struct wav_sample_reader *reader = (struct wav_sample_reader *)ctx;
    uint32_t bytes_per_sample = reader->bits_per_sample / 8;
    unsigned char sample_buf[4];

    for (uint32_t i = 0; i < sample_count; i++) {
        float sum = 0.0f;

        for (uint16_t channel = 0; channel < reader->num_channels; channel++) {
            float sample_value = 0.0f;

            if (fread(sample_buf, 1, bytes_per_sample, reader->fp) != bytes_per_sample) {
                return 1;
            }

            if (reader->bits_per_sample == 8) {
                uint8_t s = sample_buf[0];
                sample_value = ((float)s - 128.0f) / 128.0f;
            } else if (reader->bits_per_sample == 16) {
                int16_t s = (int16_t)read_le16(sample_buf);
                sample_value = (float)s / 32768.0f;
            } else if (reader->bits_per_sample == 32) {
                int32_t s = (int32_t)read_le32(sample_buf);
                sample_value = (float)s / 2147483648.0f;
            }

            if (sample_value < *min_sample) {
                *min_sample = sample_value;
            }

            if (sample_value > *max_sample) {
                *max_sample = sample_value;
            }

            sum += sample_value;
        }

        mono_samples[i] = sum / (float)reader->num_channels;
    }

    return 0;
}

static int read_alsa_mono_samples(void *ctx,
                                  float *mono_samples,
                                  uint32_t sample_count,
                                  float *min_sample,
                                  float *max_sample) {
    struct alsa_sample_reader *reader = (struct alsa_sample_reader *)ctx;
    int16_t pcm_buf[SAMPLE_BUFFER_FRAMES];
    snd_pcm_sframes_t frames_read;

    if (sample_count > SAMPLE_BUFFER_FRAMES) {
        return 1;
    }

    frames_read = snd_pcm_readi(reader->pcm, pcm_buf, sample_count);
    if (frames_read == -EPIPE) {
        if (snd_pcm_prepare(reader->pcm) < 0) {
            return 1;
        }

        frames_read = snd_pcm_readi(reader->pcm, pcm_buf, sample_count);
    }

    if (frames_read < 0 || (uint32_t)frames_read != sample_count) {
        return 1;
    }

    for (uint32_t i = 0; i < sample_count; i++) {
        float sample_value = (float)pcm_buf[i] / 32768.0f;

        if (sample_value < *min_sample) {
            *min_sample = sample_value;
        }

        if (sample_value > *max_sample) {
            *max_sample = sample_value;
        }

        mono_samples[i] = sample_value;
    }

    return 0;
}

static int detector_init(struct detector_state *state,
                         uint64_t total_frames,
                         uint16_t num_channels,
                         uint32_t sample_rate,
                         uint16_t bits_per_sample,
                         int debug) {
    memset(state, 0, sizeof(*state));

    state->num_channels = num_channels;
    state->sample_rate = sample_rate;
    state->bits_per_sample = bits_per_sample;
    state->debug = debug;

    state->bytes_per_sample = bits_per_sample / 8;
    state->frame_size = num_channels * state->bytes_per_sample;
    state->total_frames = total_frames;
    state->total_samples = total_frames * num_channels;
    state->duration_seconds = (double)total_frames / (double)sample_rate;

    state->min_sample = 1.0f;
    state->max_sample = -1.0f;

    state->target_freq = 1000.0;
    state->window_ms = 10;
    state->expected_tone_duration = 0.8;
    state->min_match_duration = 0.6;
    state->max_match_duration = 1.0;
    state->window_samples = (sample_rate * state->window_ms) / 1000;

    state->min_power = -1.0;
    state->max_power = -1.0;

    if (state->frame_size == 0) {
        fprintf(stderr, "Error: invalid frame size\n");
        return 1;
    }

    if (state->window_samples == 0) {
        state->window_samples = 1;
    }

    state->estimated_window_count = (uint32_t)(total_frames / state->window_samples);
    if (state->estimated_window_count == 0) {
        state->estimated_window_count = 1;
    }

    state->window_powers = malloc(sizeof(double) * state->estimated_window_count);
    if (state->window_powers == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        return 1;
    }

    state->tone_present = malloc(sizeof(int) * state->estimated_window_count);
    if (state->tone_present == NULL) {
        fprintf(stderr, "Error: memory allocation failed\n");
        free(state->window_powers);
        state->window_powers = NULL;
        return 1;
    }

    state->k = 0.5 + ((double)state->window_samples * state->target_freq / (double)sample_rate);
    state->omega = (2.0 * M_PI * state->k) / (double)state->window_samples;
    state->coeff = 2.0 * cos(state->omega);

    if (state->debug) {
        printf("Debug: beginning PCM sample read\n");
        printf("Debug: bytes_per_sample=%u frame_size=%u total_frames=%llu total_samples=%llu\n",
               state->bytes_per_sample,
               state->frame_size,
               (unsigned long long)state->total_frames,
               (unsigned long long)state->total_samples);
        printf("Debug: window_ms=%u window_samples=%u target_freq=%.1f\n",
               state->window_ms,
               state->window_samples,
               state->target_freq);
    }

    return 0;
}

static int detector_process_live_window(struct detector_state *state, double power) {
    double avg_power;
    double threshold;
    uint32_t current_index;
    int tone_now;
    double start_time;
    double end_time;
    double interval_duration;

    if (!state->live_stop_enabled) {
        return 0;
    }

    current_index = state->window_count - 1;
    avg_power = state->window_count > 0
        ? state->sum_power / (double)state->window_count
        : 0.0;
    threshold = avg_power;
    tone_now = power >= threshold;

    if (state->debug) {
        double window_start = ((double)current_index * (double)state->window_samples)
                            / (double)state->sample_rate;
        double window_end = ((double)(current_index + 1) * (double)state->window_samples)
                          / (double)state->sample_rate;
        printf("Debug: live_window=%u start=%.3f end=%.3f power=%.6f threshold=%.6f tone=%s\n",
               current_index,
               window_start,
               window_end,
               power,
               threshold,
               tone_now ? "present" : "absent");
    }

    if (!state->live_in_interval && tone_now) {
        state->live_in_interval = 1;
        state->live_interval_start_index = current_index;
        return 0;
    }

    if (state->live_in_interval && !tone_now) {
        start_time = ((double)state->live_interval_start_index * (double)state->window_samples)
                   / (double)state->sample_rate;
        end_time = ((double)current_index * (double)state->window_samples)
                 / (double)state->sample_rate;
        interval_duration = end_time - start_time;

        if (state->debug) {
            printf("Interval: start=%.3f end=%.3f duration=%.3f\n",
                   start_time,
                   end_time,
                   interval_duration);
        }

        state->live_in_interval = 0;

        if (interval_duration >= state->min_match_duration
                && interval_duration <= state->max_match_duration) {
            state->live_candidate_count++;
            printf("WWV 1000 Hz candidate tones:\n");
            printf("%u. start=%.3f end=%.3f duration=%.3f expected=%.3f\n",
                   state->live_candidate_count,
                   start_time,
                   end_time,
                   interval_duration,
                   state->expected_tone_duration);

            if (state->set_clock_enabled) {
                print_local_time("Current system time", state->target_time.tv_sec - 60);
                print_local_time("Setting system time", state->target_time.tv_sec);

                if (set_system_time(&state->target_time) != 0) {
                    return -1;
                }
            }

            printf("Stats: interval_count=%u candidate_count=%u\n",
                   state->live_candidate_count,
                   state->live_candidate_count);
            return 1;
        }
    }

    return 0;
}

static int detector_process_sample(struct detector_state *state, float mono_sample) {
    state->q0 = state->coeff * state->q1 - state->q2 + mono_sample;
    state->q2 = state->q1;
    state->q1 = state->q0;
    state->window_index++;

    if (state->window_index == state->window_samples) {
        double power = state->q1 * state->q1 + state->q2 * state->q2
                     - state->coeff * state->q1 * state->q2;

        if (state->window_count < state->estimated_window_count) {
            state->window_powers[state->window_count] = power;
        }

        if (state->min_power < 0.0 || power < state->min_power) {
            state->min_power = power;
        }

        if (state->max_power < 0.0 || power > state->max_power) {
            state->max_power = power;
        }

        state->sum_power += power;
        state->window_count++;

        state->q0 = 0.0;
        state->q1 = 0.0;
        state->q2 = 0.0;
        state->window_index = 0;

        return detector_process_live_window(state, power);
    }

    return 0;
}

static int detector_process_samples(struct detector_state *state,
                                    const float *mono_samples,
                                    uint32_t sample_count) {
    for (uint32_t i = 0; i < sample_count; i++) {
        int rc = detector_process_sample(state, mono_samples[i]);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static int detector_finish(struct detector_state *state) {
    state->threshold = state->window_count > 0
        ? (state->sum_power / (double)state->window_count)
        : 0.0;

    if (state->debug) {
        printf("Debug: PCM sample read complete\n");
        printf("Stats: duration_seconds=%.3f\n", state->duration_seconds);
        printf("Stats: min_sample=%.6f max_sample=%.6f\n",
               state->min_sample, state->max_sample);
        printf("Stats: window_count=%u min_power=%.6f max_power=%.6f avg_power=%.6f threshold=%.6f\n",
               state->window_count,
               state->min_power,
               state->max_power,
               state->window_count > 0 ? state->sum_power / (double)state->window_count : 0.0,
               state->threshold);
    }

    for (uint32_t i = 0; i < state->window_count && i < state->estimated_window_count; i++) {
        double window_start = ((double)i * (double)state->window_samples) / (double)state->sample_rate;
        double window_end = ((double)(i + 1) * (double)state->window_samples) / (double)state->sample_rate;

        state->tone_present[i] = state->window_powers[i] >= state->threshold;

        if (state->debug) {
            printf("Debug: window=%u start=%.3f end=%.3f power=%.6f tone=%s\n",
                   i,
                   window_start,
                   window_end,
                   state->window_powers[i],
                   state->tone_present[i] ? "present" : "absent");
        }
    }

    {
        int in_interval = 0;
        uint32_t interval_start_index = 0;
        uint32_t interval_count = 0;
        uint32_t candidate_count = 0;

        printf("WWV 1000 Hz candidate tones:\n");

        for (uint32_t i = 0; i < state->window_count && i < state->estimated_window_count; i++) {
            if (!in_interval && state->tone_present[i]) {
                in_interval = 1;
                interval_start_index = i;
            } else if (in_interval && !state->tone_present[i]) {
                double start_time = ((double)interval_start_index * (double)state->window_samples)
                                  / (double)state->sample_rate;
                double end_time = ((double)i * (double)state->window_samples)
                                / (double)state->sample_rate;
                double interval_duration = end_time - start_time;

                if (state->debug) {
                    printf("Interval: start=%.3f end=%.3f duration=%.3f\n",
                           start_time,
                           end_time,
                           interval_duration);
                }

                if (interval_duration >= state->min_match_duration
                        && interval_duration <= state->max_match_duration) {
                    candidate_count++;
                    printf("%u. start=%.3f end=%.3f duration=%.3f expected=%.3f\n",
                           candidate_count,
                           start_time,
                           end_time,
                           interval_duration,
                           state->expected_tone_duration);
                }

                interval_count++;
                in_interval = 0;
            }
        }

        if (in_interval) {
            double start_time = ((double)interval_start_index * (double)state->window_samples)
                              / (double)state->sample_rate;
            double end_time = ((double)state->window_count * (double)state->window_samples)
                            / (double)state->sample_rate;
            double interval_duration = end_time - start_time;

            if (state->debug) {
                printf("Interval: start=%.3f end=%.3f duration=%.3f\n",
                       start_time,
                       end_time,
                       interval_duration);
            }

            if (interval_duration >= state->min_match_duration
                    && interval_duration <= state->max_match_duration) {
                candidate_count++;
                printf("%u. start=%.3f end=%.3f duration=%.3f expected=%.3f\n",
                       candidate_count,
                       start_time,
                       end_time,
                       interval_duration,
                       state->expected_tone_duration);
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

    return 0;
}

static void detector_cleanup(struct detector_state *state) {
    free(state->tone_present);
    free(state->window_powers);
}

static int analyze_sample_stream(sample_reader_fn reader,
                                 void *reader_ctx,
                                 uint64_t total_frames,
                                 uint16_t num_channels,
                                 uint32_t sample_rate,
                                 uint16_t bits_per_sample,
                                 int debug,
                                 int live_stop_enabled,
                                 const struct timespec *target_time) {
    struct detector_state state;
    float mono_samples[SAMPLE_BUFFER_FRAMES];
    uint64_t frames_remaining = total_frames;

    if (detector_init(&state,
                      total_frames,
                      num_channels,
                      sample_rate,
                      bits_per_sample,
                      debug) != 0) {
        return 1;
    }

    state.live_stop_enabled = live_stop_enabled;

    if (target_time != NULL) {
        state.set_clock_enabled = 1;
        state.target_time = *target_time;
    }

    while (frames_remaining > 0) {
        uint32_t chunk_frames = frames_remaining > SAMPLE_BUFFER_FRAMES
            ? SAMPLE_BUFFER_FRAMES
            : (uint32_t)frames_remaining;

        if (reader(reader_ctx,
                   mono_samples,
                   chunk_frames,
                   &state.min_sample,
                   &state.max_sample) != 0) {
            fprintf(stderr, "Error: could not read PCM sample data\n");
            detector_cleanup(&state);
            return 1;
        }

        {
            int rc = detector_process_samples(&state, mono_samples, chunk_frames);
            if (rc > 0) {
                detector_cleanup(&state);
                return 0;
            }

            if (rc < 0) {
                detector_cleanup(&state);
                return 1;
            }
        }

        frames_remaining -= chunk_frames;
    }

    if (detector_finish(&state) != 0) {
        detector_cleanup(&state);
        return 1;
    }

    detector_cleanup(&state);
    return 0;
}

static int analyze_wav_pcm(FILE *fp,
                           long data_chunk_offset,
                           uint32_t data_chunk_size,
                           uint16_t num_channels,
                           uint32_t sample_rate,
                           uint16_t bits_per_sample,
                           int debug) {
    uint32_t bytes_per_sample = bits_per_sample / 8;
    uint32_t frame_size = num_channels * bytes_per_sample;
    uint64_t total_frames = 0;
    struct wav_sample_reader reader;

    if (fseek(fp, data_chunk_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Error: could not seek to data chunk\n");
        return 1;
    }

    total_frames = data_chunk_size / frame_size;

    reader.fp = fp;
    reader.num_channels = num_channels;
    reader.bits_per_sample = bits_per_sample;

    return analyze_sample_stream(&read_wav_mono_samples,
                                 &reader,
                                 total_frames,
                                 num_channels,
                                 sample_rate,
                                 bits_per_sample,
                                 debug,
                                 0,
                                 NULL);
}

static int analyze_alsa_pcm(const char *device_name, uint32_t capture_seconds, int debug) {
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw_params = NULL;
    struct alsa_sample_reader reader;
    struct timespec target_time;
    uint32_t sample_rate = 48000;
    uint32_t actual_rate = 0;
    uint16_t num_channels = 1;
    unsigned int actual_channels = 0;
    uint16_t bits_per_sample = 16;
    uint64_t total_frames = (uint64_t)sample_rate * capture_seconds;
    int err;
    time_t start_time;

    start_time = time(NULL);
    if (start_time == (time_t)-1) {
        fprintf(stderr, "Error: could not read current time\n");
        return 1;
    }

    print_local_time("Current system time", start_time);

    if (get_next_minute_time(&target_time) != 0) {
        return 1;
    }

    print_local_time("Target system time", target_time.tv_sec);

    err = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "Error: could not open ALSA capture device '%s': %s\n",
                device_name, snd_strerror(err));
        return 1;
    }

    snd_pcm_hw_params_alloca(&hw_params);

    err = snd_pcm_hw_params_any(pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "Error: could not initialize ALSA hardware parameters: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        fprintf(stderr, "Error: could not set ALSA access mode: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    if (err < 0) {
        fprintf(stderr, "Error: could not set ALSA sample format: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params_set_channels(pcm, hw_params, num_channels);
    if (err < 0) {
        fprintf(stderr, "Error: could not set ALSA channel count: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &sample_rate, NULL);
    if (err < 0) {
        fprintf(stderr, "Error: could not set ALSA sample rate: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    err = snd_pcm_hw_params(pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "Error: could not apply ALSA hardware parameters: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    snd_pcm_hw_params_get_rate(hw_params, &actual_rate, NULL);
    snd_pcm_hw_params_get_channels(hw_params, &actual_channels);

    if (actual_rate != sample_rate) {
        fprintf(stderr, "Error: ALSA rate mismatch (requested=%u actual=%u)\n",
                sample_rate, actual_rate);
        snd_pcm_close(pcm);
        return 1;
    }

    if (actual_channels != num_channels) {
        fprintf(stderr, "Error: ALSA channel mismatch (requested=%u actual=%u)\n",
                num_channels, actual_channels);
        snd_pcm_close(pcm);
        return 1;
    }

    if (debug) {
        printf("Debug: ALSA hw params confirmed\n");
        printf("Debug: device=%s rate=%u channels=%u format=S16_LE\n",
               device_name, actual_rate, actual_channels);
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0) {
        fprintf(stderr, "Error: could not prepare ALSA device: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }

    if (debug) {
        printf("Debug: opened ALSA capture device: %s\n", device_name);
        printf("Debug: capture_seconds=%u total_frames=%llu\n",
               capture_seconds,
               (unsigned long long)total_frames);
    }

    reader.pcm = pcm;
    reader.num_channels = num_channels;
    reader.bits_per_sample = bits_per_sample;

    err = analyze_sample_stream(&read_alsa_mono_samples,
                                &reader,
                                total_frames,
                                num_channels,
                                sample_rate,
                                bits_per_sample,
                                debug,
                                1,
                                &target_time);

    snd_pcm_close(pcm);
    return err;
}

int main(int argc, char *argv[]) {
    const char *file_path = NULL;
    const char *alsa_device = "default";
    unsigned char header[12];
    unsigned char chunk_header[RIFF_CHUNK_HEADER_SIZE];
    unsigned char fmt_data[16];
    int debug = 0;
    int use_alsa = 0;
    int found_fmt = 0;
    int found_data = 0;
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint32_t data_chunk_size = 0;
    long data_chunk_offset = 0;
    uint32_t capture_seconds = DEFAULT_ALSA_CAPTURE_SECONDS;

    static struct option long_options[] = {
        {"file", required_argument, 0, 'f'},
        {"alsa", no_argument, 0, 'a'},
        {"device", required_argument, 0, 'D'},
        {"seconds", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {"debug", no_argument, 0, 'd'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "f:aD:s:hd", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
                file_path = optarg;
                break;
            case 'a':
                use_alsa = 1;
                break;
            case 'D':
                alsa_device = optarg;
                break;
            case 's':
                capture_seconds = (uint32_t)strtoul(optarg, NULL, 10);
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

    if ((file_path == NULL && !use_alsa) || (file_path != NULL && use_alsa)) {
        usage(argv[0]);
        return 1;
    }

    if (use_alsa) {
        if (capture_seconds == 0) {
            fprintf(stderr, "Error: capture seconds must be greater than zero\n");
            return 1;
        }

        return analyze_alsa_pcm(alsa_device, capture_seconds, debug);
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

    if (analyze_wav_pcm(fp,
                        data_chunk_offset,
                        data_chunk_size,
                        num_channels,
                        sample_rate,
                        bits_per_sample,
                        debug) != 0) {
        fclose(fp);
        return 1;
    }

    fclose(fp);
    return 0;
}
