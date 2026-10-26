
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <string.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define CHANNELS 1
#define SECONDS 15
#define FRAMES_PER_BUFFER 4800

static void write_wav_header(FILE *f, uint32_t data_size)
{
    uint32_t chunk_size = 36 + data_size;
    uint16_t audio_format = 1;
    uint16_t channels = CHANNELS;
    uint32_t sample_rate = SAMPLE_RATE;
    uint16_t bits_per_sample = 32;
    uint16_t block_align = CHANNELS * 4;
    uint32_t byte_rate = SAMPLE_RATE * block_align;
    uint32_t subchunk1_size = 16;
    uint32_t subchunk2_size = data_size;

    fwrite("RIFF", 1, 4, f);
    fwrite(&chunk_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    fwrite(&subchunk1_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&subchunk2_size, 4, 1, f);
}

int main(int argc, char *argv[])
{
    const char *device = (argc > 1) ? argv[1] : "default";

    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params;
    unsigned int rate = SAMPLE_RATE;
    int dir = 0;
    int err;

    mkdir("logs", 0755);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32];

    if (!tm_info) {
        fprintf(stderr, "Unable to get timestamp\n");
        return 1;
    }

    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);

    char wav_name[128];
    snprintf(wav_name, sizeof(wav_name),
             "logs/audio/ics43434_%s.wav", timestamp);

    FILE *wav = fopen(wav_name, "wb");
    if (!wav) {
        perror("Opening WAV file");
        return 1;
    }

    /* Reserve space for the WAV header; fill it after recording. */
    unsigned char empty_header[44] = {0};
    fwrite(empty_header, 1, sizeof(empty_header), wav);

    FILE *csv = fopen("logs/audio/ics43434_levels.csv", "a");
    if (!csv) {
        perror("Opening CSV file");
        fclose(wav);
        return 1;
    }

    if (ftell(csv) == 0)
        fprintf(csv, "timestamp,elapsed_seconds,rms,peak\n");

    err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio device '%s': %s\n",
                device, snd_strerror(err));
        fclose(wav);
        fclose(csv);
        return 1;
    }

    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(
        pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(
        pcm, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, params, CHANNELS);

    err = snd_pcm_hw_params_set_rate_near(
        pcm, params, &rate, &dir);

    if (err < 0 || rate != SAMPLE_RATE) {
        fprintf(stderr, "Could not configure 48000 Hz audio\n");
        snd_pcm_close(pcm);
        fclose(wav);
        fclose(csv);
        return 1;
    }

    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        fprintf(stderr, "Audio configuration failed: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        fclose(wav);
        fclose(csv);
        return 1;
    }

    snd_pcm_prepare(pcm);

    const snd_pcm_uframes_t buffer_frames = FRAMES_PER_BUFFER;
    int32_t *buffer = malloc(
        buffer_frames * CHANNELS * sizeof(int32_t));

    if (!buffer) {
        perror("Allocating audio buffer");
        snd_pcm_close(pcm);
        fclose(wav);
        fclose(csv);
        return 1;
    }

    const uint64_t total_frames =
        (uint64_t)SAMPLE_RATE * SECONDS;

    uint64_t recorded_frames = 0;
    uint32_t data_bytes = 0;
    time_t start = time(NULL);

    printf("Recording for %d seconds...\n", SECONDS);

    while (recorded_frames < total_frames) {
        snd_pcm_uframes_t request = buffer_frames;

        if (total_frames - recorded_frames < request)
            request = total_frames - recorded_frames;

        snd_pcm_sframes_t frames = snd_pcm_readi(
            pcm, buffer, request);

        if (frames == -EPIPE) {
            fprintf(stderr, "Audio overrun; recovering...\n");
            snd_pcm_prepare(pcm);
            continue;
        }

        if (frames < 0) {
            frames = snd_pcm_recover(pcm, frames, 1);
            if (frames < 0) {
                fprintf(stderr, "Audio read failed: %s\n",
                        snd_strerror(frames));
                break;
            }
            continue;
        }

        if (frames == 0)
            continue;

        size_t samples = (size_t)frames * CHANNELS;
        size_t bytes = samples * sizeof(int32_t);

        if (fwrite(buffer, 1, bytes, wav) != bytes) {
            fprintf(stderr, "WAV write failed\n");
            break;
        }

        double sum_squares = 0.0;
        double peak = 0.0;

        for (size_t i = 0; i < samples; i++) {
            /*
             * ICS43434 data is commonly left-aligned in
             * a 32-bit I2S sample. Scale for readable levels.
             */
            double value = (double)buffer[i] / 2147483648.0;
            double magnitude = fabs(value);

            sum_squares += value * value;
            if (magnitude > peak)
                peak = magnitude;
        }

        double rms = sqrt(sum_squares / samples);
        double elapsed =
            (double)(recorded_frames + frames) / SAMPLE_RATE;

        time_t row_time = time(NULL);
        struct tm *row_tm = localtime(&row_time);
        char row_timestamp[32] = "";

        if (row_tm)
            strftime(row_timestamp, sizeof(row_timestamp),
                     "%Y-%m-%d %H:%M:%S", row_tm);

        fprintf(csv, "%s,%.3f,%.8f,%.8f\n",
                row_timestamp, elapsed, rms, peak);
        fflush(csv);

        recorded_frames += frames;
        data_bytes += (uint32_t)bytes;

        printf("\rRecorded %.2f / %d seconds",
               (double)recorded_frames / SAMPLE_RATE, SECONDS);
        fflush(stdout);
    }

    /* Rewrite the WAV header with the actual data length. */
    rewind(wav);
    write_wav_header(wav, data_bytes);

    printf("\nRecording stopped.\n");
    printf("WAV saved: %s\n", wav_name);
    printf("CSV saved: logs/audio/ics43434_levels.csv\n");

    free(buffer);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    fclose(wav);
    fclose(csv);

    return 0;
}
