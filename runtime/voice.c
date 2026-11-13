// Voice chat (voice.h). One SDL thread while the devices are open:
// - the microphone: 10 ms frames (48 kHz mono S16; SDL converts the device's own) times [Voice] MicVolume, a quiet
//   voice raised towards a target (AGC: at most 4x, slowly up and quickly down), rounded off near full scale, packed
//   by Opus (VoIP, 24 kbit/s, inband FEC) while louder than a breath (and 300 ms after; push to talk: while the key
//   or button is held, and 100 ms after), sent to the other games with ldn_send_voice;
// - the speakers: each player's packets (by the sender's address) in a jitter buffer that starts playing at 30 ms,
//   decoded in order (a missing one made up by Opus: the next packet's FEC, else PLC), all mixed at [Voice]
//   VoiceVolume, with 20..30 ms queued in the playback stream. A player silent for 100 ms stops until they are heard
//   again; one whose buffer grows past 80 ms is skipped ahead, so the voice stays near real time.
// The gate, the jitter buffer and the mixer follow MHXXRecomp's runtime/voice.c (MIT); the protocol (an 8-byte header
// of this port's own) and the transport (net.h: F_AUX, or a UDP broadcast on VOICE_UDP_PORT in crossplay) are this
// port's.
#include "voice.h"

// Opus, where the build has it: opus pkg-config adds -I<prefix>/include/opus, so the headers are reached both ways
// (Android's CMake adds include/ and include/opus/ of its own build). Without it (a machine without the library) the
// module is the stub below and voice chat stays off.
#if __has_include(<opus.h>)
#include <opus.h>
#define VOICE_HAVE_OPUS 1
#elif __has_include(<opus/opus.h>)
#include <opus/opus.h>
#define VOICE_HAVE_OPUS 1
#endif

#if !defined(VOICE_HAVE_OPUS)
// (built without Opus: voice chat stays closed, and the self-test says there is none)
void voice_tick(void) {}
void voice_aux_received(const void *data, uint32_t len) { (void)data, (void)len; }
int voice_talking(void) { return 0; }
int voice_speakers(uint32_t *ids, int max) { (void)ids, (void)max; return 0; }
bool voice_sending(void) { return false; }
int voice_selftest(void) { return -1; }
void voice_shutdown(void) {}
#else
#include <math.h>
#include <string.h>

#include <pthread.h>
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "config.h"
#include "net/net.h"
#include "rt.h"

bool host_talk_held(void);  // (host_input.c: the V key, or the controller's MISC1 button)

#define VOICE_RATE       48000
#define VOICE_FRAME      480      // samples in one 10 ms frame
#define VOICE_BITRATE    24000
#define VOICE_MAX_PACKET 400      // Opus payload bytes (30 at 24 kbit/s)
#define VOICE_HDR        8        // {magic, 0, seq LE16, sender address LE32}: then the Opus packet
#define VOICE_MAGIC      0xE9u    // (F_AUX: emsync.c's is 0xE5, hpscale.c's 0xE6, hitsync.c's 0xE7, MHXX's names 0xE8)
#define VOICE_SPEAKERS   8        // players whose voice is kept apart (each has its own decoder and jitter buffer)
#define VOICE_RING       16       // packets kept per player (160 ms)
#define VOICE_START      3        // buffered before a player starts (30 ms)
#define VOICE_SKIP       8        // buffered past this: skipped ahead (the oldest dropped: 80 ms)
#define VOICE_QUIET      10       // frames made up in a row: the player stopped (100 ms)
#define VOICE_GATE       0.015f   // louder than this (of full scale) is sent
#define VOICE_NOISE_GATE 0.02f    // ... and with [Voice] NoiseSuppression, this loud (the room stays out)
#define VOICE_AGC_TARGET 0.5f     // a voice's peaks are raised towards this
#define VOICE_AGC_MAX    4.0f     // at most this many times
#define VOICE_HANGON     30       // frames sent after the voice stops (300 ms)
#define VOICE_HANGON_PTT 10       // push to talk: the last word (100 ms)
#define VOICE_QUEUE      3        // frames kept queued in the playback stream (20..30 ms)
#define VOICE_LOUD_HZ    440.0    // voice_selftest's tone

typedef struct {  // one slot of a player's ring: their packet, or an empty one (len 0)
    uint16_t seq;
    uint16_t len;
    uint8_t data[VOICE_MAX_PACKET];
} Slot;
typedef struct {
    uint32_t id;      // 0: free
    OpusDecoder *dec;
    bool playing;
    uint16_t next;    // the next seq to play
    int made_up;      // frames made up in a row
    uint64_t heard;   // the last packet (ns)
    Slot ring[VOICE_RING];
} Speaker;

static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;  // (opening and closing: voice_tick, voice_shutdown)
static pthread_mutex_t g_rx_mu = PTHREAD_MUTEX_INITIALIZER;    // (the speakers: network threads add, the voice thread plays)
static SDL_Thread *g_thread;
static SDL_AudioStream *g_mic, *g_out;
static OpusEncoder *g_enc;
static SDL_AtomicInt g_run, g_talk, g_sending, g_ptt;
static bool g_failed;  // (an open that failed: not tried again until voice chat was off)
static float g_agc = 1.0f;  // (the microphone's automatic raise: the voice thread's)
static Speaker g_spk[VOICE_SPEAKERS];

static int16_t clamp16(float v) {
    v *= 32767.0f;
    return (int16_t)(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
}
// Up to 0.7 of full scale as it is, above it rounded off towards 1 (no hard clipping when raised)
static float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.7f) return x;
    float y = 0.7f + 0.3f * tanhf((a - 0.7f) / 0.3f);
    return x < 0 ? -y : y;
}
static void hdr_put(uint8_t *m, uint16_t seq, uint32_t id) {
    m[0] = VOICE_MAGIC, m[1] = 0;
    m[2] = (uint8_t)seq, m[3] = (uint8_t)(seq >> 8);
    m[4] = (uint8_t)id, m[5] = (uint8_t)(id >> 8), m[6] = (uint8_t)(id >> 16), m[7] = (uint8_t)(id >> 24);
}
static bool hdr_get(const uint8_t *m, uint16_t *seq, uint32_t *id) {
    if (m[0] != VOICE_MAGIC) return false;
    *seq = (uint16_t)(m[2] | m[3] << 8);
    *id = (uint32_t)m[4] | (uint32_t)m[5] << 8 | (uint32_t)m[6] << 16 | (uint32_t)m[7] << 24;
    return true;
}

// ---- Opus: a VoIP encoder for the microphone, one decoder per player
static void encoder_setup(OpusEncoder *enc) {
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(VOICE_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));  // (cheap: the game runs beside it)
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));  // (a lost packet partly rebuilt from the next one)
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
}
static bool encoder_open(void) {
    int err = 0;
    if (g_enc) return true;
    g_enc = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!g_enc) return LOG("[voice] Opus encoder: %s\n", opus_strerror(err)), false;
    encoder_setup(g_enc);
    return true;
}

// ---- receiving: a player's packet into their ring (any thread)
static void received(uint32_t id, uint16_t seq, const uint8_t *data, int len) {
    if (len <= 0 || len > VOICE_MAX_PACKET || !id) return;
    if (!SDL_GetAtomicInt(&g_run) || config_voice_mode() == VOICE_OFF) return;
    pthread_mutex_lock(&g_rx_mu);
    Speaker *s = NULL, *free_one = NULL, *oldest = &g_spk[0];
    for (int i = 0; i < VOICE_SPEAKERS; i++) {
        if (g_spk[i].id == id) s = &g_spk[i];
        if (!g_spk[i].id && !free_one) free_one = &g_spk[i];
        if (g_spk[i].heard < oldest->heard) oldest = &g_spk[i];
    }
    if (!s) {  // (a free slot, else the one heard longest ago)
        s = free_one ? free_one : oldest;
        if (!s->dec) {
            int err = 0;
            s->dec = opus_decoder_create(VOICE_RATE, 1, &err);
            if (!s->dec) {
                pthread_mutex_unlock(&g_rx_mu);
                LOG("[voice] Opus decoder: %s\n", opus_strerror(err));
                return;
            }
        } else {
            opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
        }
        s->id = id, s->playing = false, s->made_up = 0;
        memset(s->ring, 0, sizeof s->ring);
    }
    s->heard = SDL_GetTicksNS();
    if (s->playing && (int16_t)(seq - s->next) < 0) {  // (too late: already played or made up)
        pthread_mutex_unlock(&g_rx_mu);
        return;
    }
    Slot *sl = &s->ring[seq % VOICE_RING];
    sl->seq = seq, sl->len = (uint16_t)len;
    memcpy(sl->data, data, (size_t)len);
    pthread_mutex_unlock(&g_rx_mu);
}
void voice_aux_received(const void *data, uint32_t len) {
    if (len <= VOICE_HDR || len > VOICE_HDR + VOICE_MAX_PACKET) return;
    const uint8_t *m = data;
    uint16_t seq;
    uint32_t id;
    if (!hdr_get(m, &seq, &id)) return;
    if (id == ldn_own_ip()) return;  // (a crossplay room relays a broadcast back to whoever sent it)
    received(id, seq, m + VOICE_HDR, (int)(len - VOICE_HDR));
}

// ---- playing: one 10 ms frame of every player's voice, mixed (the voice thread)
// The packets waiting (from `next` on while playing; all of them before), and the earliest one's seq
static int buffered(const Speaker *s, uint16_t *lowest) {
    int n = 0;
    bool any = false;
    for (int i = 0; i < VOICE_RING; i++) {
        const Slot *sl = &s->ring[i];
        if (!sl->len || (s->playing && (int16_t)(sl->seq - s->next) < 0)) continue;
        n++;
        if (!any || (int16_t)(sl->seq - *lowest) < 0) *lowest = sl->seq;
        any = true;
    }
    return n;
}
static bool mix_frame(int16_t *out) {
    int32_t sum[VOICE_FRAME] = {0};
    int16_t pcm[VOICE_FRAME];
    int talking = 0;
    pthread_mutex_lock(&g_rx_mu);
    for (int i = 0; i < VOICE_SPEAKERS; i++) {
        Speaker *s = &g_spk[i];
        if (!s->id || !s->dec) continue;
        uint16_t lowest = 0;
        int n = buffered(s, &lowest);
        if (!s->playing) {
            if (n < VOICE_START) continue;
            s->playing = true, s->next = lowest, s->made_up = 0;
        } else if (n > VOICE_SKIP) {  // (behind: catch up, the oldest dropped)
            s->next = (uint16_t)(lowest + (n - VOICE_START));
        }
        Slot *sl = &s->ring[s->next % VOICE_RING];
        int got;
        if (sl->len && sl->seq == s->next) {
            got = opus_decode(s->dec, sl->data, sl->len, pcm, VOICE_FRAME, 0);
            sl->len = 0, s->made_up = 0;
        } else {
            Slot *after = &s->ring[(uint16_t)(s->next + 1) % VOICE_RING];
            got = after->len && after->seq == (uint16_t)(s->next + 1)
                      ? opus_decode(s->dec, after->data, after->len, pcm, VOICE_FRAME, 1)  // (from the next one's FEC)
                      : opus_decode(s->dec, NULL, 0, pcm, VOICE_FRAME, 0);                  // (lost: Opus makes it up)
            if (++s->made_up >= VOICE_QUIET) {  // (stopped talking: what is left is stale)
                s->playing = false;
                memset(s->ring, 0, sizeof s->ring);
            }
        }
        s->next++;
        if (got == VOICE_FRAME) {
            for (int k = 0; k < VOICE_FRAME; k++) sum[k] += pcm[k];
            if (!s->made_up) talking++;
        }
    }
    pthread_mutex_unlock(&g_rx_mu);
    SDL_SetAtomicInt(&g_talk, talking);
    bool any = false;
    float gain = config_voice_play_gain();  // (here, not the stream's gain: past full scale it is rounded off, not cut)
    for (int k = 0; k < VOICE_FRAME; k++) {
        any |= sum[k] != 0;
        out[k] = clamp16(soft_clip(sum[k] * gain / 32768.0f));
    }
    return any;
}

static int SDLCALL run(void *unused) {
    (void)unused;
    int16_t pcm[VOICE_FRAME], mixed[VOICE_FRAME];
    uint8_t msg[VOICE_HDR + VOICE_MAX_PACKET];
    int hangon = 0;
    uint16_t seq = 0;
    while (SDL_GetAtomicInt(&g_run)) {
        bool idle = true;
        // the microphone
        if (g_mic && SDL_GetAudioStreamAvailable(g_mic) >= (int)sizeof pcm &&
            SDL_GetAudioStreamData(g_mic, pcm, sizeof pcm) == (int)sizeof pcm) {
            idle = false;
            int mode = config_voice_mode();
            bool noise = config_voice_noise() != 0;
            float gain = config_voice_mic_gain(), raw = 0.0f, peak = 0.0f;
            for (int i = 0; i < VOICE_FRAME; i++) {
                float a = fabsf(pcm[i] * gain) / 32768.0f;
                if (a > raw) raw = a;
            }
            // a quiet voice raised towards VOICE_AGC_TARGET at its loudest (at most VOICE_AGC_MAX times; slowly up,
            // quickly down); [Voice] NoiseSuppression keeps the room's noise out of that
            if (raw > (noise ? VOICE_NOISE_GATE : VOICE_GATE)) {
                float want = VOICE_AGC_TARGET / raw;
                want = want < 1.0f ? 1.0f : want > VOICE_AGC_MAX ? VOICE_AGC_MAX : want;
                g_agc += (want - g_agc) * (want < g_agc ? 0.5f : 0.01f);
            }
            for (int i = 0; i < VOICE_FRAME; i++) {
                float v = soft_clip(pcm[i] * gain * g_agc / 32768.0f);
                pcm[i] = clamp16(v);
                float a = fabsf(v);
                if (a > peak) peak = a;
            }
            bool loud = noise ? raw > VOICE_NOISE_GATE : peak > VOICE_GATE;
            bool ptt = mode == VOICE_PTT && SDL_GetAtomicInt(&g_ptt) != 0;
            if (ptt || (mode == VOICE_ALWAYS && loud)) hangon = mode == VOICE_PTT ? VOICE_HANGON_PTT : VOICE_HANGON;
            else if (hangon) hangon--;
            bool net = mode != VOICE_OFF && ldn_net_up();
            SDL_SetAtomicInt(&g_sending, net && hangon > 0);
            if (net && hangon > 0) {
                if (noise && !loud && !ptt) memset(pcm, 0, sizeof pcm);  // (nothing of the room sent)
                int n = opus_encode(g_enc, pcm, VOICE_FRAME, msg + VOICE_HDR, VOICE_MAX_PACKET);
                if (n > 0) {
                    hdr_put(msg, seq, ldn_own_ip());
                    ldn_send_voice(msg, (uint32_t)(VOICE_HDR + (size_t)n));
                }
            }
            seq++;
        }
        // the speakers: kept 20..30 ms ahead
        if (g_out && SDL_GetAudioStreamQueued(g_out) < VOICE_QUEUE * (int)sizeof mixed) {
            if (mix_frame(mixed)) SDL_PutAudioStreamData(g_out, mixed, sizeof mixed), idle = false;
        }
        if (idle) SDL_Delay(2);
    }
    return 0;
}

static void close_all(void) {  // (g_open_mu held)
    if (g_thread) {
        SDL_SetAtomicInt(&g_run, 0);
        SDL_WaitThread(g_thread, NULL);
        g_thread = NULL;
        LOG("[voice] closed\n");
    }
    if (g_mic) SDL_DestroyAudioStream(g_mic), g_mic = NULL;
    if (g_out) SDL_DestroyAudioStream(g_out), g_out = NULL;
    pthread_mutex_lock(&g_rx_mu);
    for (int i = 0; i < VOICE_SPEAKERS; i++) {
        if (g_spk[i].dec) opus_decoder_destroy(g_spk[i].dec);
        memset(&g_spk[i], 0, sizeof g_spk[i]);
    }
    pthread_mutex_unlock(&g_rx_mu);
    SDL_SetAtomicInt(&g_talk, 0);
    SDL_SetAtomicInt(&g_sending, 0);
    SDL_SetAtomicInt(&g_ptt, 0);
    g_agc = 1.0f;
}
static bool open_all(void) {  // (g_open_mu held)
    bool ok = true;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 1, VOICE_RATE};
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOG("[voice] no sound: %s\n", SDL_GetError());
        ok = false;
    } else if (!encoder_open()) {
        ok = false;
    } else if (!(g_mic = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL))) {
        LOG("[voice] no microphone: %s\n", SDL_GetError());
        ok = false;
    } else if (!(g_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL))) {
        LOG("[voice] no sound device: %s\n", SDL_GetError());
        ok = false;
    }
    if (ok) {
        SDL_ResumeAudioStreamDevice(g_mic);
        SDL_ResumeAudioStreamDevice(g_out);
        SDL_SetAtomicInt(&g_run, 1);
        if (!(g_thread = SDL_CreateThread(run, "voice", NULL))) {
            LOG("[voice] the voice thread did not start: %s\n", SDL_GetError());
            SDL_SetAtomicInt(&g_run, 0);
            ok = false;
        }
    }
    if (!ok) {
        close_all();
        return false;
    }
    LOG("[voice] open: %s -> %s\n", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_mic)),
        SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_out)));
    return true;
}

#ifdef __ANDROID__
// The phone's microphone is a runtime permission (AndroidManifest's RECORD_AUDIO): GameActivity asks for it, and the
// capture is opened only once it is granted, so a voice chat switched on before the answer waits rather than failing.
// (The java methods are GameActivity's, static, called from this thread as the menu's are from ui.c.)
static bool g_mic_permitted, g_mic_asked;  // (granted, and the dialog asked for once this run)
static uint64_t g_mic_next;                // (checked again at most once a second, not every frame)
static bool mic_permission(bool ask) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return false;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = cls ? (*env)->GetStaticMethodID(env, cls, ask ? "requestMicrophone" : "hasMicrophonePermission",
                                                  ask ? "()V" : "()Z") : NULL;
    jboolean granted = JNI_FALSE;
    if (m && ask) (*env)->CallStaticVoidMethod(env, cls, m);
    else if (m) granted = (*env)->CallStaticBooleanMethod(env, cls, m);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return granted == JNI_TRUE;
}
// True while the capture may be opened, asking for the permission the first time it is missing.
static bool mic_allowed(void) {
    uint64_t now = SDL_GetTicksNS();
    if (now >= g_mic_next) {
        g_mic_next = now + 1000000000ull;
        g_mic_permitted = mic_permission(false);
        if (!g_mic_permitted && !g_mic_asked) {
            g_mic_asked = true;
            LOG("[voice] asking for the microphone (RECORD_AUDIO)\n");
            mic_permission(true);
        }
    }
    return g_mic_permitted;
}
#endif

void voice_tick(void) {
    // (read here, on the host's thread, beside the game's own input)
    SDL_SetAtomicInt(&g_ptt, host_talk_held() ? 1 : 0);
    bool want = config_voice_mode() != VOICE_OFF && ldn_net_up();
    pthread_mutex_lock(&g_open_mu);
    if (want && !g_thread && !g_failed) {
#ifdef __ANDROID__
        if (mic_allowed()) g_failed = !open_all();
#else
        g_failed = !open_all();
#endif
    } else if (!want && g_thread) {
        close_all();
    }
    if (!want) g_failed = false;  // (a network that comes back, or a mode set again, tries once more)
    pthread_mutex_unlock(&g_open_mu);
}

int voice_talking(void) { return SDL_GetAtomicInt(&g_talk); }
bool voice_sending(void) { return SDL_GetAtomicInt(&g_sending) != 0; }
int voice_speakers(uint32_t *ids, int max) {
    int n = 0;
    pthread_mutex_lock(&g_rx_mu);
    for (int i = 0; i < VOICE_SPEAKERS && n < max; i++)
        if (g_spk[i].id && g_spk[i].playing && !g_spk[i].made_up) ids[n++] = g_spk[i].id;
    pthread_mutex_unlock(&g_rx_mu);
    return n;
}

// A tone through Opus in this build (a check from the settings), with an encoder and decoder of its own: a voice
// session that is running is left alone. A packet in seven is dropped, so the FEC and PLC paths are checked too.
int voice_selftest(void) {
    int err = 0, last = -1, heard = 0;
    OpusEncoder *enc = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc) return LOG("[voice] Opus encoder: %s\n", opus_strerror(err)), -1;
    encoder_setup(enc);
    OpusDecoder *dec = opus_decoder_create(VOICE_RATE, 1, &err);
    if (!dec) {
        opus_encoder_destroy(enc);
        return LOG("[voice] Opus decoder: %s\n", opus_strerror(err)), -1;
    }
    int16_t pcm[VOICE_FRAME], back[VOICE_FRAME];
    uint8_t packet[VOICE_MAX_PACKET];
    for (int f = 0; f < 50; f++) {  // (half a second)
        for (int i = 0; i < VOICE_FRAME; i++)
            pcm[i] = (int16_t)(8000.0 * sin(2.0 * 3.14159265358979 * VOICE_LOUD_HZ * (f * VOICE_FRAME + i) / VOICE_RATE));
        int n = opus_encode(enc, pcm, VOICE_FRAME, packet, sizeof packet);
        if (n <= 0) {
            last = -1;
            break;
        }
        last = n;
        if (f % 7 == 3) continue;  // (lost: the next one's FEC, or made up)
        int got = opus_decode(dec, packet, n, back, VOICE_FRAME, 0);
        if (got == VOICE_FRAME)
            for (int i = 0; i < VOICE_FRAME; i++)
                if (back[i] > 2000 || back[i] < -2000) heard = 1;
    }
    opus_decoder_destroy(dec);
    opus_encoder_destroy(enc);
    if (last < 0 || !heard) LOG("[voice] the self-test heard nothing of its own tone\n");
    return last < 0 || !heard ? -1 : last;
}

void voice_shutdown(void) {
    pthread_mutex_lock(&g_open_mu);
    close_all();
    if (g_enc) opus_encoder_destroy(g_enc), g_enc = NULL;
    pthread_mutex_unlock(&g_open_mu);
}
#endif
