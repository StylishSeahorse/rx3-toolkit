/* SPDX-License-Identifier: MPL-2.0
 * Minimal ALSA device contract for the RX3 firmware emulator.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RX3_AUDIO_MAGIC 0x52583341u

struct rx3_audio_handle {
    uint32_t magic;
    snd_pcm_stream_t stream;
    unsigned int rate;
    unsigned int channels;
    snd_pcm_format_t format;
    snd_pcm_uframes_t period;
    int raw_fd;
    unsigned int endpoint;
    unsigned long long frames;
};

static unsigned int pending_rate = 44100u;
static unsigned int pending_channels = 2u;
static snd_pcm_format_t pending_format = SND_PCM_FORMAT_S16_LE;
static snd_pcm_uframes_t pending_period = 512u;
static char output_directory[256] = "/tmp/rx3emu";
static int initialized;
static void log_line(const char *format, ...);

struct rx3_pcm_info {
    int device;
    int subdevice;
    int stream;
};

static void initialize(void)
{
    if (initialized)
        return;
    const char *configured = getenv("RX3EMU_OUTPUT");
    if (configured && configured[0]) {
        strncpy(output_directory, configured, sizeof(output_directory) - 1u);
        output_directory[sizeof(output_directory) - 1u] = '\0';
    }
    initialized = 1;
}

__attribute__((constructor)) static void audio_shim_loaded(void)
{
    initialize();
    log_line("audio-shim-loaded");
}

static void log_line(const char *format, ...)
{
    initialize();
    char path[320];
    snprintf(path, sizeof(path), "%s/audio.log", output_directory);
    FILE *stream = fopen(path, "a");
    if (!stream)
        return;
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stream, format, arguments);
    va_end(arguments);
    fputc('\n', stream);
    fclose(stream);
}

static struct rx3_audio_handle *audio_handle(snd_pcm_t *pcm)
{
    struct rx3_audio_handle *handle = (struct rx3_audio_handle *)pcm;
    return handle && handle->magic == RX3_AUDIO_MAGIC ? handle : NULL;
}

static unsigned int sample_bytes(snd_pcm_format_t format)
{
    switch (format) {
    case SND_PCM_FORMAT_S8:
    case SND_PCM_FORMAT_U8:
        return 1u;
    case SND_PCM_FORMAT_S16_LE:
    case SND_PCM_FORMAT_S16_BE:
    case SND_PCM_FORMAT_U16_LE:
    case SND_PCM_FORMAT_U16_BE:
        return 2u;
    case SND_PCM_FORMAT_S24_3LE:
    case SND_PCM_FORMAT_S24_3BE:
    case SND_PCM_FORMAT_U24_3LE:
    case SND_PCM_FORMAT_U24_3BE:
        return 3u;
    default:
        return 4u;
    }
}

static void pace(const struct rx3_audio_handle *handle,
                 snd_pcm_uframes_t frames)
{
    if (!handle->rate)
        return;
    struct timespec delay;
    unsigned long long nanoseconds =
        (unsigned long long)frames * 1000000000ull / handle->rate;
    delay.tv_sec = (time_t)(nanoseconds / 1000000000ull);
    delay.tv_nsec = (long)(nanoseconds % 1000000000ull);
    nanosleep(&delay, NULL);
}

static void publish_metadata(const struct rx3_audio_handle *handle)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/audio-playback-%u.json",
             output_directory, handle->endpoint);
    FILE *stream = fopen(path, "w");
    if (!stream)
        return;
    fprintf(stream,
            "{\n  \"rate\": %u,\n  \"channels\": %u,\n"
            "  \"format\": %d,\n  \"sample_bytes\": %u,\n"
            "  \"frames\": %llu\n}\n",
            handle->rate, handle->channels, (int)handle->format,
            sample_bytes(handle->format), handle->frames);
    fclose(stream);
}

int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
    (void)mode;
    if (!ctl || !name || strncmp(name, "hw:", 3u)) {
        log_line("ctl-reject %s", name ? name : "(null)");
        return -ENOENT;
    }
    *ctl = (snd_ctl_t *)calloc(1u, 16u);
    log_line("ctl-open %s", name);
    return *ctl ? 0 : -ENOMEM;
}

int snd_ctl_close(snd_ctl_t *ctl)
{
    free(ctl);
    return 0;
}

int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info)
{
    (void)ctl;
    struct rx3_pcm_info *request = (struct rx3_pcm_info *)info;
    int result = request && request->device == 0 && request->subdevice == 0
        ? 0 : -ENOENT;
    log_line("ctl-pcm-info device=%d subdevice=%d stream=%d result=%d",
             request ? request->device : -1,
             request ? request->subdevice : -1,
             request ? request->stream : -1, result);
    return result;
}

void snd_pcm_info_set_stream(snd_pcm_info_t *info, snd_pcm_stream_t stream)
{ ((struct rx3_pcm_info *)info)->stream = (int)stream; }
void snd_pcm_info_set_device(snd_pcm_info_t *info, unsigned int device)
{ ((struct rx3_pcm_info *)info)->device = (int)device; }
void snd_pcm_info_set_subdevice(snd_pcm_info_t *info, unsigned int subdevice)
{ ((struct rx3_pcm_info *)info)->subdevice = (int)subdevice; }

int snd_pcm_open(snd_pcm_t **pcm, const char *name,
                 snd_pcm_stream_t stream, int mode)
{
    (void)mode;
    if (!pcm || !name)
        return -EINVAL;
    struct rx3_audio_handle *handle = calloc(1u, sizeof(*handle));
    if (!handle)
        return -ENOMEM;
    handle->magic = RX3_AUDIO_MAGIC;
    handle->stream = stream;
    handle->rate = pending_rate;
    handle->channels = pending_channels;
    handle->format = pending_format;
    handle->period = pending_period;
    handle->raw_fd = -1;
    const char *separator = strrchr(name, ',');
    handle->endpoint = separator ? (unsigned int)atoi(separator + 1) : 0u;
    if (stream == SND_PCM_STREAM_PLAYBACK) {
        initialize();
        char path[320];
        snprintf(path, sizeof(path), "%s/audio-playback-%u.raw",
                 output_directory, handle->endpoint);
        handle->raw_fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    }
    *pcm = (snd_pcm_t *)handle;
    log_line("pcm-open %s stream=%d", name, (int)stream);
    return 0;
}

int snd_pcm_close(snd_pcm_t *pcm)
{
    struct rx3_audio_handle *handle = audio_handle(pcm);
    if (!handle)
        return -EINVAL;
    if (handle->raw_fd >= 0)
        close(handle->raw_fd);
    handle->magic = 0u;
    free(handle);
    return 0;
}

size_t snd_pcm_hw_params_sizeof(void) { log_line("hw-size"); return 512u; }
size_t snd_pcm_sw_params_sizeof(void) { log_line("sw-size"); return 512u; }
size_t snd_pcm_info_sizeof(void) { log_line("info-size"); return 512u; }
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{ (void)pcm; memset(params, 0, 512u); return 0; }
int snd_pcm_hw_params_get_channels_min(const snd_pcm_hw_params_t *p, unsigned int *v)
{ (void)p; if (v) *v = 1u; return 0; }
int snd_pcm_hw_params_get_channels_max(const snd_pcm_hw_params_t *p, unsigned int *v)
{ (void)p; if (v) *v = 32u; return 0; }
int snd_pcm_hw_params_test_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                unsigned int rate, int direction)
{ (void)pcm; (void)p; (void)direction; return rate >= 8000u && rate <= 192000u ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                 snd_pcm_access_t access)
{ (void)pcm; (void)p; (void)access; return 0; }
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                 snd_pcm_format_t format)
{ (void)p; struct rx3_audio_handle *h = audio_handle(pcm); if (h) h->format = format; pending_format = format; return 0; }
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                    unsigned int *rate, int *direction)
{ (void)p; (void)direction; struct rx3_audio_handle *h = audio_handle(pcm); if (rate) { pending_rate = *rate; if (h) h->rate = *rate; } return 0; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                   unsigned int channels)
{ (void)p; struct rx3_audio_handle *h = audio_handle(pcm); if (h) h->channels = channels; pending_channels = channels; return 0; }
int snd_pcm_hw_params_set_periods_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                       unsigned int *periods, int *direction)
{ (void)pcm; (void)p; (void)periods; (void)direction; return 0; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p,
                                           snd_pcm_uframes_t *frames, int *direction)
{ (void)p; (void)direction; struct rx3_audio_handle *h = audio_handle(pcm); if (frames) { pending_period = *frames; if (h) h->period = *frames; } return 0; }
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *params)
{
    (void)params;
    struct rx3_audio_handle *handle = audio_handle(pcm);
    if (!handle) return -EINVAL;
    log_line("pcm-config stream=%d rate=%u channels=%u format=%d period=%lu",
             (int)handle->stream, handle->rate, handle->channels,
             (int)handle->format, (unsigned long)handle->period);
    if (handle->stream == SND_PCM_STREAM_PLAYBACK)
        publish_metadata(handle);
    return 0;
}

int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{ (void)pcm; memset(params, 0, 512u); return 0; }
int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *p, snd_pcm_uframes_t *v)
{ (void)p; if (v) *v = 0x3fffffffu; return 0; }
int snd_pcm_sw_params_set_silence_threshold(snd_pcm_t *a, snd_pcm_sw_params_t *b, snd_pcm_uframes_t c)
{ (void)a; (void)b; (void)c; return 0; }
int snd_pcm_sw_params_set_silence_size(snd_pcm_t *a, snd_pcm_sw_params_t *b, snd_pcm_uframes_t c)
{ (void)a; (void)b; (void)c; return 0; }
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *a, snd_pcm_sw_params_t *b, snd_pcm_uframes_t c)
{ (void)a; (void)b; (void)c; return 0; }
int snd_pcm_sw_params_set_stop_threshold(snd_pcm_t *a, snd_pcm_sw_params_t *b, snd_pcm_uframes_t c)
{ (void)a; (void)b; (void)c; return 0; }
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *params)
{ (void)pcm; (void)params; return 0; }
int snd_pcm_prepare(snd_pcm_t *pcm) { return audio_handle(pcm) ? 0 : -EINVAL; }
int snd_pcm_link(snd_pcm_t *a, snd_pcm_t *b)
{ return audio_handle(a) && audio_handle(b) ? 0 : -EINVAL; }

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer,
                                snd_pcm_uframes_t frames)
{
    struct rx3_audio_handle *handle = audio_handle(pcm);
    if (!handle) return -EINVAL;
    memset(buffer, 0, (size_t)frames * handle->channels * sample_bytes(handle->format));
    pace(handle, frames);
    return (snd_pcm_sframes_t)frames;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer,
                                 snd_pcm_uframes_t frames)
{
    struct rx3_audio_handle *handle = audio_handle(pcm);
    if (!handle) return -EINVAL;
    size_t bytes = (size_t)frames * handle->channels * sample_bytes(handle->format);
    if (handle->raw_fd >= 0 && write(handle->raw_fd, buffer, bytes) != (ssize_t)bytes)
        return -EIO;
    handle->frames += frames;
    if ((handle->frames & 0x3ffffu) < frames)
        publish_metadata(handle);
    pace(handle, frames);
    return (snd_pcm_sframes_t)frames;
}

const char *snd_strerror(int error)
{
    (void)error;
    return "RX3 emulator ALSA bridge";
}
