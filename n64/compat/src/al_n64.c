/* OpenAL on libdragon's mixer: the part of OpenAL 1.1 STK uses.
 *
 * On other platforms STK's sound goes OpenAL -> MojoAL (a software mixer)
 * -> SDL audio, and STK decodes every sound effect from Ogg Vorbis into a
 * buffer of PCM samples. On the N64 neither fits: the decoded effects alone
 * would be tens of MB, and decoding Vorbis and mixing in floating point
 * would take a CPU the game already needs whole. So this file replaces
 * MojoAL (lib/mojoal/CMakeLists.txt builds it instead on the N64):
 *
 *   - a buffer is a libdragon .wav64 file on the cartridge (the asset
 *     pipeline converts every .ogg, n64/tools/stk_n64_assets.py), streamed
 *     by the mixer and decoded on the RSP. STK attaches the file with the
 *     extension alBufferWav64N64() (src/audio/sfx_buffer.cpp,
 *     music_wav64.cpp); alBufferData() is accepted and ignored;
 *   - a playing source takes a mixer channel (two for a stereo file); its
 *     gain, pitch and stereo position are recomputed from OpenAL's default
 *     distance model (inverse distance, clamped) and the listener whenever
 *     one of them changes;
 *   - the game's main thread keeps the mixer fed (stk_n64_audio_service).
 *
 * Calls come from several of STK's threads (the SFX manager has one): one
 * kernel mutex covers the state and the mixer.
 */
#include <libdragon.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "AL/al.h"
#include "AL/alc.h"

#define MAX_BUFFERS  256
#define MAX_SOURCES  128
#define MIX_CHANNELS 24
#define OUTPUT_RATE  32000

typedef struct
{
    int      used;
    int      open;          /* has a wav64 */
    wav64_t  wav;
} n64_buffer_t;

typedef struct
{
    int      used;
    ALuint   buffer;
    ALint    state;         /* AL_INITIAL, AL_PLAYING, AL_PAUSED, AL_STOPPED */
    int      ch;            /* mixer channel, -1 = none */
    int      looping, relative;
    float    gain, pitch, rolloff, ref_dist, max_dist;
    float    pos[3];
    double   paused_pos;    /* in samples, while AL_PAUSED */
} n64_source_t;

static n64_buffer_t g_buf[MAX_BUFFERS];
static n64_source_t g_src[MAX_SOURCES];
static int          g_ch_owner[MIX_CHANNELS];     /* source index + 1 */
static float        g_listener_pos[3];
static float        g_listener_at[3] = { 0, 0, -1 }, g_listener_up[3] = { 0, 1, 0 };
static float        g_listener_gain = 1.0f;
static ALenum       g_error = AL_NO_ERROR;
static kmutex_t     g_lock;
static int          g_up;

/* sounds started and mixer buffers written, for the heap log's heartbeat */
unsigned stk_n64_al_plays, stk_n64_al_buffers;

static void set_error(ALenum e) { if (g_error == AL_NO_ERROR) g_error = e; }

/* ----------------------------------------------------------------- mixer -- */
/* The mixer is fed from the game's own thread (the SDL back-end calls this
 * whenever it is called itself: event polls, texture uploads, frame swaps;
 * STK's loading screens come through there too). An audio thread of its own
 * was tried first: libdragon's RSP queue and its users are not made for
 * two threads, and the game froze while sounds were being loaded. The price
 * is that a frame longer than the audio buffer (160 ms) is heard as a gap. */
void stk_n64_audio_service(void)
{
    if (!g_up)
        return;
    kmutex_lock(&g_lock);
    while (audio_can_write())
    {
        short *buf = audio_write_begin();
        mixer_poll(buf, audio_get_buffer_length());
        audio_write_end();
        stk_n64_al_buffers++;
    }
    kmutex_unlock(&g_lock);
}

static n64_buffer_t *buffer_of(ALuint id)
{
    return (id >= 1 && id <= MAX_BUFFERS && g_buf[id - 1].used)
        ? &g_buf[id - 1] : NULL;
}

static n64_source_t *source_of(ALuint id)
{
    return (id >= 1 && id <= MAX_SOURCES && g_src[id - 1].used)
        ? &g_src[id - 1] : NULL;
}

/* channels of a source that the mixer finished with are given back */
static void reap(n64_source_t *s)
{
    if (s->ch >= 0 && s->state == AL_PLAYING && !mixer_ch_playing(s->ch))
    {
        const int n = (buffer_of(s->buffer) &&
                       buffer_of(s->buffer)->wav.wave.channels == 2) ? 2 : 1;
        for (int i = 0; i < n; i++)
            g_ch_owner[s->ch + i] = 0;
        s->ch = -1;
        s->state = AL_STOPPED;
    }
}

static void release_channel(n64_source_t *s)
{
    if (s->ch < 0)
        return;
    mixer_ch_stop(s->ch);
    const n64_buffer_t *b = buffer_of(s->buffer);
    const int n = (b && b->wav.wave.channels == 2) ? 2 : 1;
    for (int i = 0; i < n; i++)
        g_ch_owner[s->ch + i] = 0;
    s->ch = -1;
}

static int alloc_channel(int n, int owner)
{
    for (int pass = 0; pass < 2; pass++)
    {
        for (int ch = 0; ch + n <= MIX_CHANNELS; ch++)
        {
            int ok = 1;
            for (int i = 0; i < n && ok; i++)
            {
                const int o = g_ch_owner[ch + i];
                if (o && pass == 1)
                    reap(&g_src[o - 1]);
                ok = g_ch_owner[ch + i] == 0;
            }
            if (ok)
            {
                for (int i = 0; i < n; i++)
                    g_ch_owner[ch + i] = owner;
                return ch;
            }
        }
    }
    return -1;
}

/* gain and stereo position of a source, from OpenAL's default distance
 * model (AL_INVERSE_DISTANCE_CLAMPED) and the listener */
static void apply_volume(n64_source_t *s)
{
    if (s->ch < 0)
        return;
    float d[3];
    for (int i = 0; i < 3; i++)
        d[i] = s->relative ? s->pos[i] : s->pos[i] - g_listener_pos[i];
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float gain = s->gain * g_listener_gain;
    if (s->rolloff > 0.0f)
    {
        float cd = dist < s->ref_dist ? s->ref_dist : dist;
        if (cd > s->max_dist) cd = s->max_dist;
        gain *= s->ref_dist / (s->ref_dist + s->rolloff * (cd - s->ref_dist));
    }
    float pan = 0.0f;             /* -1 left .. 1 right */
    if (dist > 0.0001f)
    {
        /* right = at x up */
        const float *a = g_listener_at, *u = g_listener_up;
        const float r[3] = { a[1] * u[2] - a[2] * u[1],
                             a[2] * u[0] - a[0] * u[2],
                             a[0] * u[1] - a[1] * u[0] };
        const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (rl > 0.0001f)
            pan = (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (dist * rl);
    }
    if (gain > 1.0f) gain = 1.0f;
    const n64_buffer_t *b = buffer_of(s->buffer);
    if (b && b->wav.wave.channels == 2)
        mixer_ch_set_vol(s->ch, gain, gain);      /* music: as recorded */
    else
        mixer_ch_set_vol(s->ch, gain * sqrtf((1.0f - pan) * 0.5f),
                         gain * sqrtf((1.0f + pan) * 0.5f));
}

static void apply_pitch(n64_source_t *s)
{
    const n64_buffer_t *b = buffer_of(s->buffer);
    if (s->ch >= 0 && b)
        mixer_ch_set_freq(s->ch, b->wav.wave.frequency * s->pitch);
}

static void source_play(n64_source_t *s, int index)
{
    n64_buffer_t *b = buffer_of(s->buffer);
    release_channel(s);
    if (!b || !b->open)
    {
        s->state = AL_STOPPED;          /* nothing to play: as if it ended */
        return;
    }
    const int n = b->wav.wave.channels == 2 ? 2 : 1;
    const int ch = alloc_channel(n, index + 1);
    if (ch < 0)
    {
        s->state = AL_STOPPED;          /* every channel busy: dropped */
        return;
    }
    s->ch = ch;
    stk_n64_al_plays++;
    mixer_ch_play(ch, &b->wav.wave);
    mixer_ch_set_loop(ch, s->looping);
    s->state = AL_PLAYING;
    apply_pitch(s);
    apply_volume(s);
}

/* ------------------------------------------------------------------- ALC -- */
static int g_device, g_context;

ALCdevice *alcOpenDevice(const ALCchar *name)
{
    (void)name;
    if (!g_up)
    {
        kmutex_init(&g_lock, KMUTEX_STANDARD);
        audio_init(OUTPUT_RATE, AUDIO_DEFAULT_LATENCY);
        mixer_init(MIX_CHANNELS);
        wav64_init_compression(2);          /* ULC: the pipeline's codec */
        g_up = 1;
    }
    return (ALCdevice *)&g_device;
}

ALCboolean alcCloseDevice(ALCdevice *d) { (void)d; return ALC_TRUE; }
ALCcontext *alcCreateContext(ALCdevice *d, const ALCint *attr)
{
    (void)d; (void)attr;
    return (ALCcontext *)&g_context;
}
void alcDestroyContext(ALCcontext *c) { (void)c; }
ALCboolean alcMakeContextCurrent(ALCcontext *c) { (void)c; return ALC_TRUE; }
ALCcontext *alcGetCurrentContext(void) { return g_up ? (ALCcontext *)&g_context : NULL; }
ALCdevice *alcGetContextsDevice(ALCcontext *c) { (void)c; return (ALCdevice *)&g_device; }

ALenum alGetError(void)
{
    const ALenum e = g_error;
    g_error = AL_NO_ERROR;
    return e;
}

/* --------------------------------------------------------------- buffers -- */
void alGenBuffers(ALsizei n, ALuint *ids)
{
    kmutex_lock(&g_lock);
    for (ALsizei k = 0; k < n; k++)
    {
        ids[k] = 0;
        for (int i = 0; i < MAX_BUFFERS; i++)
            if (!g_buf[i].used)
            {
                memset(&g_buf[i], 0, sizeof(g_buf[i]));
                g_buf[i].used = 1;
                ids[k] = i + 1;
                break;
            }
        if (!ids[k])
            set_error(AL_OUT_OF_MEMORY);
    }
    kmutex_unlock(&g_lock);
}

void alDeleteBuffers(ALsizei n, const ALuint *ids)
{
    kmutex_lock(&g_lock);
    for (ALsizei k = 0; k < n; k++)
    {
        n64_buffer_t *b = buffer_of(ids[k]);
        if (!b)
            continue;
        for (int i = 0; i < MAX_SOURCES; i++)
            if (g_src[i].used && g_src[i].buffer == ids[k])
            {
                release_channel(&g_src[i]);
                g_src[i].buffer = 0;
                g_src[i].state = AL_STOPPED;
            }
        if (b->open)
            wav64_close(&b->wav);
        b->used = b->open = 0;
    }
    kmutex_unlock(&g_lock);
}

ALboolean alIsBuffer(ALuint id) { return buffer_of(id) ? AL_TRUE : AL_FALSE; }

/* PCM from a decoder: not used on the N64 (see the top of this file) */
void alBufferData(ALuint id, ALenum format, const ALvoid *data, ALsizei size,
                  ALsizei freq)
{
    (void)id; (void)format; (void)data; (void)size; (void)freq;
}

/* N64 extension: the buffer plays a .wav64 file. Returns its duration in
 * seconds, or a negative number if it cannot be opened. */
float alBufferWav64N64(ALuint id, const char *path)
{
    kmutex_lock(&g_lock);
    n64_buffer_t *b = buffer_of(id);
    float duration = -1.0f;
    FILE *f = b ? fopen(path, "rb") : NULL;     /* wav64_open asserts */
    if (f)
    {
        fclose(f);
        if (b->open)
            wav64_close(&b->wav);
        wav64_open(&b->wav, path);
        b->open = 1;
        /* every buffer can loop (whole file): sources choose per play */
        if (b->wav.wave.loop_len == 0)
            b->wav.wave.loop_len = b->wav.wave.len;
        duration = (float)b->wav.wave.len / b->wav.wave.frequency;
    }
    kmutex_unlock(&g_lock);
    return duration;
}

/* N64 extension: loop region of a buffer, in seconds (music) */
void alBufferLoopN64(ALuint id, float start, float end)
{
    kmutex_lock(&g_lock);
    n64_buffer_t *b = buffer_of(id);
    if (b && b->open)
    {
        waveform_t *w = &b->wav.wave;
        int e = end > 0.0f ? (int)(end * w->frequency) : w->len;
        if (e > w->len || e <= 0) e = w->len;
        int st = (int)(start * w->frequency);
        if (st < 0 || st >= e) st = 0;
        w->loop_end = e;
        w->loop_len = e - st;
    }
    kmutex_unlock(&g_lock);
}

/* --------------------------------------------------------------- sources -- */
void alGenSources(ALsizei n, ALuint *ids)
{
    kmutex_lock(&g_lock);
    for (ALsizei k = 0; k < n; k++)
    {
        ids[k] = 0;
        for (int i = 0; i < MAX_SOURCES; i++)
            if (!g_src[i].used)
            {
                n64_source_t *s = &g_src[i];
                memset(s, 0, sizeof(*s));
                s->used = 1;
                s->ch = -1;
                s->state = AL_INITIAL;
                s->gain = s->pitch = s->rolloff = s->ref_dist = 1.0f;
                s->max_dist = 3.4e38f;
                ids[k] = i + 1;
                break;
            }
        if (!ids[k])
            set_error(AL_OUT_OF_MEMORY);
    }
    kmutex_unlock(&g_lock);
}

void alDeleteSources(ALsizei n, const ALuint *ids)
{
    kmutex_lock(&g_lock);
    for (ALsizei k = 0; k < n; k++)
    {
        n64_source_t *s = source_of(ids[k]);
        if (!s)
            continue;
        release_channel(s);
        s->used = 0;
    }
    kmutex_unlock(&g_lock);
}

ALboolean alIsSource(ALuint id) { return source_of(id) ? AL_TRUE : AL_FALSE; }

void alSourcei(ALuint id, ALenum p, ALint v)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (!s)
        set_error(AL_INVALID_NAME);
    else if (p == AL_BUFFER)
    {
        release_channel(s);
        s->buffer = (ALuint)v;
        s->state = AL_INITIAL;
    }
    else if (p == AL_LOOPING)
    {
        s->looping = v != 0;
        if (s->ch >= 0)
            mixer_ch_set_loop(s->ch, s->looping);
    }
    else if (p == AL_SOURCE_RELATIVE)
    {
        s->relative = v != 0;
        apply_volume(s);
    }
    kmutex_unlock(&g_lock);
}

void alSourcef(ALuint id, ALenum p, ALfloat v)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (!s)
        set_error(AL_INVALID_NAME);
    else
    {
        switch (p)
        {
        case AL_GAIN:               s->gain = v;     apply_volume(s); break;
        case AL_PITCH:              s->pitch = v;    apply_pitch(s);  break;
        case AL_ROLLOFF_FACTOR:     s->rolloff = v;  apply_volume(s); break;
        case AL_REFERENCE_DISTANCE: s->ref_dist = v; apply_volume(s); break;
        case AL_MAX_DISTANCE:       s->max_dist = v; apply_volume(s); break;
        default: break;
        }
    }
    kmutex_unlock(&g_lock);
}

void alSource3f(ALuint id, ALenum p, ALfloat x, ALfloat y, ALfloat z)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (!s)
        set_error(AL_INVALID_NAME);
    else if (p == AL_POSITION)
    {
        s->pos[0] = x; s->pos[1] = y; s->pos[2] = z;
        apply_volume(s);
    }
    kmutex_unlock(&g_lock);
}

void alGetSourcei(ALuint id, ALenum p, ALint *v)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (!s)
        set_error(AL_INVALID_NAME);
    else if (p == AL_SOURCE_STATE)
    {
        reap(s);
        *v = s->state;
    }
    else if (p == AL_BUFFERS_PROCESSED || p == AL_BUFFERS_QUEUED)
        *v = 0;
    else
        *v = 0;
    kmutex_unlock(&g_lock);
}

void alSourcePlay(ALuint id)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (!s)
        set_error(AL_INVALID_NAME);
    else if (s->state == AL_PAUSED)
    {
        /* resume where it was */
        source_play(s, (int)(id - 1));
        if (s->ch >= 0)
            mixer_ch_set_pos(s->ch, s->paused_pos);
    }
    else
        source_play(s, (int)(id - 1));
    kmutex_unlock(&g_lock);
}

void alSourceStop(ALuint id)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (s)
    {
        release_channel(s);
        s->state = AL_STOPPED;
    }
    kmutex_unlock(&g_lock);
}

void alSourcePause(ALuint id)
{
    kmutex_lock(&g_lock);
    n64_source_t *s = source_of(id);
    if (s && s->state == AL_PLAYING && s->ch >= 0)
    {
        s->paused_pos = mixer_ch_get_pos(s->ch);
        release_channel(s);
        s->state = AL_PAUSED;
    }
    kmutex_unlock(&g_lock);
}

/* queued streaming (MusicOggStream): not used on the N64 (music_wav64.cpp) */
void alSourceQueueBuffers(ALuint id, ALsizei n, const ALuint *b)
{
    (void)id; (void)n; (void)b;
}

void alSourceUnqueueBuffers(ALuint id, ALsizei n, ALuint *b)
{
    (void)id;
    for (ALsizei i = 0; i < n; i++)
        b[i] = 0;
}

/* -------------------------------------------------------------- listener -- */
void alListener3f(ALenum p, ALfloat x, ALfloat y, ALfloat z)
{
    kmutex_lock(&g_lock);
    if (p == AL_POSITION)
    {
        g_listener_pos[0] = x; g_listener_pos[1] = y; g_listener_pos[2] = z;
        for (int i = 0; i < MAX_SOURCES; i++)
            if (g_src[i].used && g_src[i].ch >= 0)
                apply_volume(&g_src[i]);
    }
    kmutex_unlock(&g_lock);
}

void alListenerfv(ALenum p, const ALfloat *v)
{
    kmutex_lock(&g_lock);
    if (p == AL_ORIENTATION)
    {
        memcpy(g_listener_at, v, 3 * sizeof(float));
        memcpy(g_listener_up, v + 3, 3 * sizeof(float));
    }
    else if (p == AL_POSITION)
        memcpy(g_listener_pos, v, 3 * sizeof(float));
    kmutex_unlock(&g_lock);
}

void alListenerf(ALenum p, ALfloat v)
{
    if (p == AL_GAIN)
        g_listener_gain = v;
}
