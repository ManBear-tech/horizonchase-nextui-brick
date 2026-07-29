/*
 * opensles_shim.c -- OpenSL ES to SDL2 audio bridge
 *
 * Translates OpenSL ES BufferQueue audio players into SDL2 audio output.
 * Lock-free SPSC ring buffers between game thread and SDL audio thread.
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#define MAX_PLAYERS 16
#define RING_BUFFER_SIZE (4 * 1024 * 1024)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
/* s14: 2048 amostras (~46ms). A tentativa 4096 + ring-fundo(6x) + max_calls=32 fez a
 * thread do pump VIRAR moedor de CPU (chamava o mixer do FMOD 32x/ciclo) -> travou o
 * device + distorceu (feedback "bugou bonito"). Voltei pro simples; a folga de
 * verdade vem do FIX de performance (draws sem glGetIntegerv) que tira o thrash. */
#define SDL_AUDIO_SAMPLES 2048

/* Interface ID storage */
static const int id_engine_tag = 1;
static const int id_play_tag = 2;
static const int id_volume_tag = 3;
static const int id_bufferqueue_tag = 4;
static const int id_effectsend_tag = 5;
static const int id_enginecap_tag = 6;
static const int id_envreverb_tag = 7;
static const int id_androidcfg_tag = 8;

const SLInterfaceID sl_IID_ENGINE = &id_engine_tag;
const SLInterfaceID sl_IID_PLAY = &id_play_tag;
const SLInterfaceID sl_IID_VOLUME = &id_volume_tag;
const SLInterfaceID sl_IID_BUFFERQUEUE = &id_bufferqueue_tag;
const SLInterfaceID sl_IID_EFFECTSEND = &id_effectsend_tag;
const SLInterfaceID sl_IID_ENGINECAPABILITIES = &id_enginecap_tag;
const SLInterfaceID sl_IID_ENVIRONMENTALREVERB = &id_envreverb_tag;
/* FMOD (Unity) exige SL_IID_ANDROIDCONFIGURATION no init (s14) — identidade própria;
   GetInterface devolve stub-success (config de stream type/latency é só hint) */
const SLInterfaceID sl_IID_ANDROIDCONFIGURATION = &id_androidcfg_tag;

/* OpenSL ES data structures */
typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

/* Per-player state */
typedef void (*slBufferQueueCallback)(void *caller, void *pContext);

typedef struct {
  uint8_t ring[RING_BUFFER_SIZE];
  volatile uint32_t ring_head;
  volatile uint32_t ring_tail;

  uint32_t queued_sizes[64];
  volatile uint32_t queued_head_index;
  volatile uint32_t queued_tail_index;
  volatile uint32_t queued_count;
  volatile uint32_t queued_front_offset;
  uint32_t queue_capacity;

  slBufferQueueCallback callback;
  void *callback_context;

  uint32_t enqueued_since_cb;
  uint32_t last_enqueue_size;

  void (*play_callback)(void *caller, void *pContext, SLuint32 event);
  void *play_callback_context;
  SLuint32 play_event_mask;

  uint32_t enqueue_counter;
  uint32_t debug_enqueue_logs;
  uint32_t debug_callback_logs;
  uint32_t debug_play_callback_logs;
  int ever_enqueued;
  int headatend_fired;
  int decoder_done;
  volatile uint32_t underrun_count;
  volatile uint32_t fadeout_count;
  volatile uint32_t frames_played;  /* total output frames mixed, for fade-in */

  volatile SLuint32 play_state;
  float volume;
  int active;
  uint64_t played_bytes;

  SLuint32 num_channels;
  SLuint32 sample_rate;
  SLuint32 bits_per_sample;

  void *obj_vtable[8];
  void *obj_ptr;
  void *play_vtable[8];
  void *play_ptr;
  void *volume_vtable[8];
  void *volume_ptr;
  void *bq_vtable[8];
  void *bq_ptr;
  void *effectsend_vtable[8];
  void *effectsend_ptr;
} AudioPlayer;

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;

static void queue_reset(AudioPlayer *p) {
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->played_bytes = 0;
}

static void queue_push(AudioPlayer *p, uint32_t size) {
  if (size == 0) return;

  if (p->queued_count >= (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]))) {
    uint32_t tail = (p->queued_tail_index - 1) %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    p->queued_sizes[tail] += size;
    return;
  }

  uint32_t tail = p->queued_tail_index %
                  (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
  p->queued_sizes[tail] = size;
  p->queued_tail_index++;
  p->queued_count++;
}

static void queue_consume(AudioPlayer *p, uint32_t bytes) {
  while (bytes > 0 && p->queued_count > 0) {
    uint32_t head = p->queued_head_index %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    uint32_t queued = p->queued_sizes[head];
    uint32_t remaining = queued - p->queued_front_offset;

    if (bytes < remaining) {
      p->queued_front_offset += bytes;
      return;
    }

    bytes -= remaining;
    p->queued_sizes[head] = 0;
    p->queued_head_index++;
    p->queued_count--;
    p->queued_front_offset = 0;
  }
}

/* Ring buffer helpers */
static inline uint32_t ring_readable(const AudioPlayer *p) {
  return p->ring_head - p->ring_tail;
}

static inline uint32_t ring_writable(const AudioPlayer *p) {
  return RING_BUFFER_SIZE - (p->ring_head - p->ring_tail);
}

static uint32_t ring_write(AudioPlayer *p, const void *data, uint32_t len) {
  uint32_t space = ring_writable(p);
  if (len > space) len = space;
  if (len == 0) return 0;

  const uint8_t *src = (const uint8_t *)data;
  uint32_t head = p->ring_head & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - head;
  if (first > len) first = len;
  memcpy(p->ring + head, src, first);
  if (len > first) memcpy(p->ring, src + first, len - first);

  __sync_synchronize();
  p->ring_head += len;
  return len;
}

static uint32_t ring_read(AudioPlayer *p, void *data, uint32_t len) {
  uint32_t avail = ring_readable(p);
  if (len > avail) len = avail;
  if (len == 0) return 0;

  uint8_t *dst = (uint8_t *)data;
  uint32_t tail = p->ring_tail & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - tail;
  if (first > len) first = len;
  memcpy(dst, p->ring + tail, first);
  if (len > first) memcpy(dst + first, p->ring, len - first);

  __sync_synchronize();
  p->ring_tail += len;
  return len;
}

/* SDL2 audio callback */
#define SDL_OUTPUT_RATE 44100
#define TMP_BUF_SAMPLES (SDL_AUDIO_SAMPLES * 2)

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  memset(stream, 0, len);

  int16_t *out = (int16_t *)stream;
  int out_samples = len / (int)sizeof(int16_t);

  static float mix_buf[SDL_AUDIO_SAMPLES * 2];
  if (out_samples > SDL_AUDIO_SAMPLES * 2) out_samples = SDL_AUDIO_SAMPLES * 2;
  memset(mix_buf, 0, out_samples * sizeof(float));

  uint32_t out_frames = out_samples / 2;

  /* Per-player temp buffer on stack */
  int16_t tmp[TMP_BUF_SAMPLES];

  /* Per-player diagnostics for click detection */
  float player_peak[MAX_PLAYERS];
  float player_vol[MAX_PLAYERS];
  int player_active_list[MAX_PLAYERS];
  int num_active = 0;
  memset(player_peak, 0, sizeof(player_peak));
  memset(player_vol, 0, sizeof(player_vol));

  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t src_rate = p->sample_rate;
    if (src_rate == 0) src_rate = SDL_OUTPUT_RATE;
    uint32_t src_channels = p->num_channels;
    if (src_channels == 0) src_channels = 2;
    uint32_t frame_size = src_channels * sizeof(int16_t);
    float vol = p->volume;
    /* Guard against corrupted volume */
    if (vol < 0.0f || vol > 2.0f || vol != vol /* NaN */) {
      static uint32_t vol_warn = 0;
      if (vol_warn < 20) {
        debugPrintf("opensles_shim: CORRUPT vol player %d: volume=%f (raw bits=0x%08x)\n",
                    i, vol, *(uint32_t*)&vol);
        vol_warn++;
      }
      vol = 0.0f; /* mute corrupted player */
    }
    if (src_channels == 1) {
      vol *= 0.35f;
    } else {
      vol *= 0.8f;
    }
    player_vol[i] = vol;
    player_active_list[num_active++] = i;

    uint32_t src_frames_needed;
    if (src_rate == SDL_OUTPUT_RATE) {
      src_frames_needed = out_frames;
    } else {
      src_frames_needed = (uint32_t)((uint64_t)out_frames * src_rate / SDL_OUTPUT_RATE) + 2;
    }

    uint32_t src_bytes_want = src_frames_needed * frame_size;
    if (src_bytes_want > sizeof(tmp)) src_bytes_want = sizeof(tmp);
    src_bytes_want = (src_bytes_want / frame_size) * frame_size;

    uint32_t got = ring_read(p, tmp, src_bytes_want);
    got = (got / frame_size) * frame_size;
    uint32_t src_frames_got = got / frame_size;
    if (src_frames_got == 0) continue;
    queue_consume(p, got);
    p->played_bytes += got;

    /* Detect underrun: got less than requested = fade out last frames to avoid click */
    int underrun = (got < src_bytes_want);
    uint32_t fade_out_len = 64;
    uint32_t fade_start = 0;
    if (underrun) {
      p->underrun_count++;
      if (src_frames_got > fade_out_len) {
        fade_start = src_frames_got - fade_out_len;
      } else {
        /* Very short buffer: fade the entire thing */
        fade_start = 0;
        fade_out_len = src_frames_got;
      }
      p->fadeout_count++;
    }

    /* Fade-in: first 32 output frames of a player's lifetime */
    uint32_t fadein_remaining = (p->frames_played < 32) ? (32 - p->frames_played) : 0;

    if (src_rate == SDL_OUTPUT_RATE && src_channels == 2) {
      uint32_t n = src_frames_got;
      if (n > out_frames) n = out_frames;
      for (uint32_t f = 0; f < n; f++) {
        float env = 1.0f;
        /* Fade-in */
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        /* Fade-out on underrun */
        if (underrun && f >= fade_start && fade_out_len > 0) {
          float fo = 1.0f - (float)(f - fade_start) / (float)fade_out_len;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        float cl = (float)tmp[f * 2]     * v;
        float cr = (float)tmp[f * 2 + 1] * v;
        mix_buf[f * 2]     += cl;
        mix_buf[f * 2 + 1] += cr;
        float ap = fabsf(cl);
        if (ap > player_peak[i]) player_peak[i] = ap;
        ap = fabsf(cr);
        if (ap > player_peak[i]) player_peak[i] = ap;
      }
      p->frames_played += n;
    } else {
      uint32_t step = (uint32_t)((uint64_t)src_rate * 65536 / SDL_OUTPUT_RATE);
      uint32_t pos = 0;
      /* Map fade_start from source frames to output frames */
      uint32_t fade_start_out = underrun ?
        (uint32_t)((uint64_t)fade_start * SDL_OUTPUT_RATE / src_rate) : out_frames + 1;
      uint32_t fade_out_len_out = (uint32_t)((uint64_t)fade_out_len * SDL_OUTPUT_RATE / src_rate);
      if (fade_out_len_out == 0) fade_out_len_out = 1;
      uint32_t mixed = 0;
      for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t idx = pos >> 16;
        uint32_t frac = pos & 0xFFFF;
        if (idx >= src_frames_got) break;

        float l0, r0, l1, r1;
        if (src_channels == 1) {
          l0 = (float)tmp[idx]; r0 = l0;
          l1 = (idx + 1 < src_frames_got) ? (float)tmp[idx + 1] : l0; r1 = l1;
        } else {
          l0 = (float)tmp[idx * 2]; r0 = (float)tmp[idx * 2 + 1];
          if (idx + 1 < src_frames_got) { l1 = (float)tmp[(idx + 1) * 2]; r1 = (float)tmp[(idx + 1) * 2 + 1]; }
          else { l1 = l0; r1 = r0; }
        }

        float frac_f = (float)frac / 65536.0f;
        float left  = l0 + (l1 - l0) * frac_f;
        float right = r0 + (r1 - r0) * frac_f;
        float env = 1.0f;
        /* Fade-in */
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        /* Fade-out on underrun */
        if (f >= fade_start_out && fade_out_len_out > 0) {
          float fo = 1.0f - (float)(f - fade_start_out) / (float)fade_out_len_out;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        float cl = left * v;
        float cr = right * v;
        mix_buf[f * 2]     += cl;
        mix_buf[f * 2 + 1] += cr;
        float ap = fabsf(cl);
        if (ap > player_peak[i]) player_peak[i] = ap;
        ap = fabsf(cr);
        if (ap > player_peak[i]) player_peak[i] = ap;
        pos += step;
        mixed++;
      }
      p->frames_played += mixed;
    }
  }

  /* Soft-clip using tanh-style limiter - smooth, no discontinuities */
  const float master_gain = 0.30f;
  const float threshold = 28000.0f;
  const float knee = 4000.0f;  /* transition zone */
  static int16_t prev_left = 0, prev_right = 0;
  static uint32_t click_count = 0;
  static uint32_t callback_count = 0;
  callback_count++;

  for (int s = 0; s < out_samples; s++) {
    float x = mix_buf[s] * master_gain;
    float ax = fabsf(x);
    if (ax > threshold) {
      float over = ax - threshold;
      float compressed = threshold + knee * (over / (over + knee));
      x = (x > 0) ? compressed : -compressed;
    }
    if (x > 32767.0f) x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    out[s] = (int16_t)x;
  }

  /* Detect clicks: large jump between last sample of previous buffer
   * and first sample of this buffer */
  int32_t jump_l = abs((int32_t)out[0] - (int32_t)prev_left);
  int32_t jump_r = abs((int32_t)out[1] - (int32_t)prev_right);
  /* Also detect mix_buf peak */
  float mix_peak = 0.0f;
  for (int s = 0; s < out_samples; s++) {
    float av = fabsf(mix_buf[s]);
    if (av > mix_peak) mix_peak = av;
  }
  if (jump_l > 8000 || jump_r > 8000) {
    click_count++;
    debugPrintf("opensles_shim: CLICK #%u cb#%u jump L=%d R=%d prev=%d/%d new=%d/%d mixpeak=%.0f active=%d\n",
                click_count, callback_count,
                jump_l, jump_r,
                (int)prev_left, (int)prev_right,
                (int)out[0], (int)out[1],
                mix_peak, num_active);
    for (int a = 0; a < num_active; a++) {
      int pi = player_active_list[a];
      AudioPlayer *pp = &g_players[pi];
      debugPrintf("  p%d: vol=%.3f peak=%.0f ch=%u rate=%u readable=%u\n",
                  pi, player_vol[pi], player_peak[pi],
                  pp->num_channels, pp->sample_rate, ring_readable(pp));
    }
  }
  if (out_samples >= 2) {
    prev_left = out[out_samples - 2];
    prev_right = out[out_samples - 1];
  }
}

/* Initialize SDL2 audio */
int opensles_shim_engine_active(void) { return g_audio_initialized && g_audio_dev != 0; }
/* s14 (Cuphead/FMOD): o System::init do FMOD roda DENTRO do nativeRender e BLOQUEIA
 * esperando os callbacks da buffer queue — mas o pump era chamado só pelo render
 * loop (a MESMA thread!) -> os 2 primeiros Enqueue nunca drenavam -> timeout ->
 * "failed to initialize the output device". Pump em THREAD PRÓPRIA (modelo real do
 * OpenSL: callbacks numa thread de áudio); o hook do render loop vira no-op. */
static void pump_callbacks_impl(void);
static volatile int g_pump_thread_on;
static void *sl_pump_thread(void *arg) {
  (void)arg;
  fprintf(stderr, "[SL] pump thread ativa (4ms)\n");
  while (1) { pump_callbacks_impl(); usleep(4000); }
  return NULL;
}
static void ensure_audio_initialized(void) {
  if (g_audio_initialized) return;

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = SDL_AUDIO_SAMPLES;
  want.callback = sdl_audio_callback;
  want.userdata = NULL;

  if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    fprintf(stderr, "[SL] SDL_InitSubSystem(AUDIO): %s\n", SDL_GetError());
  g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (g_audio_dev == 0) {
    fprintf(stderr, "[SL] SDL_OpenAudioDevice FALHOU: %s (driver=%s)\n",
            SDL_GetError(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    g_audio_initialized = 1;
    return;
  }

  fprintf(stderr, "[SL] SDL audio aberto: %dHz %dch %d samples (driver=%s)\n",
          have.freq, have.channels, have.samples,
          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_audio_initialized = 1;
  if (!g_pump_thread_on) {
    g_pump_thread_on = 1;
    pthread_t pt; pthread_create(&pt, NULL, sl_pump_thread, NULL);
    pthread_detach(pt);
  }
}

/* Reset player metadata without touching the 4MB ring buffer.
 * The ring head/tail tracking ensures we never read unwritten data. */
static void player_reset_meta(AudioPlayer *p) {
  p->ring_head = 0;
  p->ring_tail = 0;
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->queue_capacity = 0;
  p->callback = NULL;
  p->callback_context = NULL;
  p->enqueued_since_cb = 0;
  p->last_enqueue_size = 0;
  p->play_callback = NULL;
  p->play_callback_context = NULL;
  p->play_event_mask = 0;
  p->enqueue_counter = 0;
  p->debug_enqueue_logs = 0;
  p->debug_callback_logs = 0;
  p->debug_play_callback_logs = 0;
  p->ever_enqueued = 0;
  p->headatend_fired = 0;
  p->decoder_done = 0;
  p->underrun_count = 0;
  p->fadeout_count = 0;
  p->frames_played = 0;
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->volume = 1.0f;
  p->active = 1;
  p->played_bytes = 0;
  p->num_channels = 0;
  p->sample_rate = 0;
  p->bits_per_sample = 0;
}

/* Allocate a player */
static AudioPlayer *alloc_player(void) {
  pthread_mutex_lock(&g_players_lock);
  /* 1. Find inactive player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g_players[i].active) {
      AudioPlayer *p = &g_players[i];
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: allocated player %d\n", i); */
      return p;
    }
  }
  /* 2. Recycle stopped + drained player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED &&
        ring_readable(p) == 0 &&
        p->queued_count == 0) {
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: recycled player %d\n", i); */
      return p;
    }
  }
  /* 3. Recycle any stopped player */
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED) {
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      /* debugPrintf("opensles_shim: force-recycled stopped player %d\n", i); */
      return p;
    }
  }
  /* 4. Force-kill oldest playing player (last resort) */
  {
    int oldest = -1;
    uint64_t most_played = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      AudioPlayer *p = &g_players[i];
      if (p->played_bytes >= most_played) {
        most_played = p->played_bytes;
        oldest = i;
      }
    }
    if (oldest >= 0) {
      AudioPlayer *p = &g_players[oldest];
      debugPrintf("opensles_shim: WARNING: force-killing player %d (state=%u played=%llu)\n",
                  oldest, p->play_state, (unsigned long long)p->played_bytes);
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  pthread_mutex_unlock(&g_players_lock);
  debugPrintf("opensles_shim: FATAL: no player slots at all!\n");
  return NULL;
}

/* SLPlayItf methods */
static SLresult play_GetDuration(void *self, SLmillisecond *pMsec) {
  (void)self;
  if (pMsec) *pMsec = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPosition(void *self, SLmillisecond *pMsec) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      uint64_t position = 0;
      uint32_t channels = p->num_channels ? p->num_channels : 2;
      uint32_t sample_rate = p->sample_rate ? p->sample_rate : 44100;
      uint32_t bytes_per_frame = channels * sizeof(int16_t);
      if (bytes_per_frame != 0 && sample_rate != 0) {
        uint64_t frames = p->played_bytes / bytes_per_frame;
        position = (frames * 1000ULL) / sample_rate;
      }
      if (pMsec) *pMsec = (SLmillisecond)position;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pMsec) *pMsec = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      { static int n; if (n++ < 12) fprintf(stderr, "[SL] SetPlayState p%d %u->%u\n", i, p->play_state, state); }
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      if (state == SL_PLAYSTATE_STOPPED && p->play_state != SL_PLAYSTATE_STOPPED) {
        p->headatend_fired = 0;
        p->decoder_done = 0;
        p->ring_head = 0;
        p->ring_tail = 0;
        queue_reset(p);
      }
      if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING) {
        p->frames_played = 0;
        p->underrun_count = 0;
        p->fadeout_count = 0;
      }
      p->play_state = state;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      if (pState) *pState = g_players[i].play_state;
      { static int n; if (n++ < 6) fprintf(stderr, "[SL] GetPlayState p%d -> %u\n", i, g_players[i].play_state); }
      return SL_RESULT_SUCCESS;
    }
  }
  if (pState) *pState = SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}

static SLresult play_RegisterCallback(void *self, void *callback, void *ctx) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      { static int n; if (n++ < 4) fprintf(stderr, "[SL] play_RegisterCallback p%d cb=%p\n", i, callback); }
      g_players[i].play_callback = (void (*)(void *, void *, SLuint32))callback;
      g_players[i].play_callback_context = ctx;
      /* if (text_base && ra >= (uintptr_t)text_base &&
          ra < (uintptr_t)text_base + text_size) {
        debugPrintf("opensles_shim: player %d play callback registered=%p ctx=%p caller=game.so+0x%lx\n",
                    i, callback, ctx, (unsigned long)(ra - (uintptr_t)text_base));
      } else {
        debugPrintf("opensles_shim: player %d play callback registered=%p ctx=%p caller=%p\n",
                    i, callback, ctx, (void *)ra);
      } */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(void *self, SLuint32 eventFlags) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_event_mask = eventFlags;
      /* debugPrintf("opensles_shim: player %d play event mask=0x%x\n", i, eventFlags); */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

/* SLVolumeItf methods */
static SLresult volume_SetVolumeLevel(void *self, SLmillibel level) {
  float linear;
  if (level <= -9600) linear = 0.0f;
  else linear = powf(10.0f, level / 2000.0f);

  /* Clamp insane values */
  if (linear > 2.0f) {
    debugPrintf("opensles_shim: WARNING: SetVolumeLevel level=%d -> linear=%f, clamping to 1.0\n",
                (int)level, linear);
    linear = 1.0f;
  }

  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].volume_ptr == itf_ptr) {
      /* debugPrintf("opensles_shim: player %d SetVolumeLevel(%d) -> %f\n", i, (int)level, linear); */
      g_players[i].volume = linear;
      return SL_RESULT_SUCCESS;
    }
  }
  debugPrintf("opensles_shim: WARNING: SetVolumeLevel - no matching player for itf=%p\n", self);
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  (void)self;
  if (pLevel) *pLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if (pMaxLevel) *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

/* SLBufferQueueItf methods */
static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  void **itf_ptr = (void **)self;
  { static int n; if (n++ < 16) fprintf(stderr, "[SL] bq_Enqueue #%d size=%u (PCM fluindo!)\n", n, (unsigned)size); }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      uint32_t written = ring_write(p, pBuffer, size);
      if (written != size) {
        debugPrintf("opensles_shim: WARNING: truncated enqueue for player %d (%u/%u bytes)\n",
                    i, written, size);
      }
      queue_push(p, written);
      p->enqueued_since_cb += written;
      if (written > 0) {
        p->last_enqueue_size = written;
        p->enqueue_counter++;
        p->ever_enqueued = 1;
        /* if (p->debug_enqueue_logs < 16 || p->enqueue_counter % 64 == 0) {
          debugPrintf("opensles_shim: player %d enqueue size=%u written=%u readable=%u counter=%u\n",
                      i, size, written, ring_readable(p), p->enqueue_counter);
          p->debug_enqueue_logs++;
        } */
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      /* debugPrintf("opensles_shim: player %d BufferQueue Clear\n", i); */
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->ring_head = 0;
      p->ring_tail = 0;
      queue_reset(p);
      p->enqueued_since_cb = 0;
      p->enqueue_counter = 0;
      p->ever_enqueued = 0;
      p->headatend_fired = 0;
      p->decoder_done = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (pState) {
        SLuint32 *state = (SLuint32 *)pState;
        uint32_t play_index = 0;
        uint32_t capacity = p->queue_capacity ? p->queue_capacity : 1;
        if (p->queued_count > 0) {
          play_index = p->queued_head_index % capacity;
        }
        state[0] = p->queued_count;
        state[1] = play_index;
        { static int n; if (n++ < 12) fprintf(stderr, "[SL] bq_GetState p%d count=%u idx=%u\n", i, state[0], state[1]); }
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback callback, void *pContext) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      { static int n; if (n++ < 4) fprintf(stderr, "[SL] bq_RegisterCallback p%d cb=%p\n", i, callback); }
      p->callback = callback;
      p->callback_context = pContext;
      /* if (text_base && ra >= (uintptr_t)text_base &&
          ra < (uintptr_t)text_base + text_size) {
        debugPrintf("opensles_shim: player %d buffer callback registered=%p ctx=%p caller=game.so+0x%lx\n",
                    i, callback, pContext, (unsigned long)(ra - (uintptr_t)text_base));
      } else {
        debugPrintf("opensles_shim: player %d buffer callback registered=%p ctx=%p caller=%p\n",
                    i, callback, pContext, (void *)ra);
      } */
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState_or_RegisterCallback(void *self, void *arg1, void *arg2) {
  uintptr_t maybe_callback = (uintptr_t)arg1;
  uintptr_t text = (uintptr_t)text_base;

  if (text_base && maybe_callback >= text && maybe_callback < text + text_size) {
    return bq_RegisterCallback(self, (slBufferQueueCallback)arg1, arg2);
  }

  return bq_GetState(self, arg1);
}

/* Stub for unused interfaces */
static SLresult stub_success(void) { return SL_RESULT_SUCCESS; }
/* s14: recurso de engine não suportado (CreateAudioRecorder/Midi/LED...) */
static SLresult engine_Unsupported(void) {
  static int n; if (n++ < 6) fprintf(stderr, "[SL] engine itf não-suportado chamado -> FEATURE_UNSUPPORTED\n");
  return 12 /* SL_RESULT_FEATURE_UNSUPPORTED */;
}

/* GetState (SLObjectItf vtable[2]): FMOD confere o estado pós-Realize; o
 * stub_success não escrevia *pState -> lixo -> init abortava ("FMOD failed to
 * initialize the output device"). Sempre REALIZED(2). */
static SLresult obj_GetState(void *self, SLuint32 *pState) {
  (void)self;
  if (pState) *pState = 2 /* SL_OBJECT_STATE_REALIZED */;
  return SL_RESULT_SUCCESS;
}

/* Player object methods */
static SLresult player_Realize(void *self, SLBoolean async) {
  (void)self; (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (iid == sl_IID_PLAY) {
        /* debugPrintf("opensles_shim: player %d GetInterface(PLAY)\n", i); */
        *pInterface = &p->play_ptr;
      } else if (iid == sl_IID_VOLUME) {
        /* debugPrintf("opensles_shim: player %d GetInterface(VOLUME)\n", i); */
        *pInterface = &p->volume_ptr;
      } else if (iid == sl_IID_BUFFERQUEUE) {
        /* debugPrintf("opensles_shim: player %d GetInterface(BUFFERQUEUE)\n", i); */
        *pInterface = &p->bq_ptr;
      } else if (iid == sl_IID_EFFECTSEND) {
        /* debugPrintf("opensles_shim: player %d GetInterface(EFFECTSEND)\n", i); */
        *pInterface = &p->effectsend_ptr;
      } else {
        { static int n; if (n++ < 8) fprintf(stderr, "[SL] player%d GetInterface(iid=%p desconhecido)\n", i, (void *)iid); }
        *pInterface = &p->effectsend_ptr;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      p->active = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return;
    }
  }
}

/* Setup player vtables */
static void setup_player_vtables(AudioPlayer *p) {
  for (int i = 0; i < 8; i++) p->obj_vtable[i] = (void *)stub_success;
  p->obj_vtable[0] = (void *)player_Realize;
  p->obj_vtable[2] = (void *)obj_GetState;
  p->obj_vtable[3] = (void *)player_GetInterface;
  p->obj_vtable[6] = (void *)player_Destroy;
  p->obj_ptr = p->obj_vtable;

  for (int i = 0; i < 8; i++) p->play_vtable[i] = (void *)stub_success;
  p->play_vtable[0] = (void *)play_SetPlayState;
  p->play_vtable[1] = (void *)play_GetPlayState;
  p->play_vtable[2] = (void *)play_GetDuration;
  p->play_vtable[3] = (void *)play_GetPosition;
  p->play_vtable[4] = (void *)play_RegisterCallback;
  p->play_vtable[5] = (void *)play_SetCallbackEventsMask;
  p->play_ptr = p->play_vtable;

  for (int i = 0; i < 8; i++) p->volume_vtable[i] = (void *)stub_success;
  p->volume_vtable[0] = (void *)volume_SetVolumeLevel;
  p->volume_vtable[1] = (void *)volume_GetVolumeLevel;
  p->volume_vtable[2] = (void *)volume_GetMaxVolumeLevel;
  p->volume_ptr = p->volume_vtable;

  for (int i = 0; i < 8; i++) p->bq_vtable[i] = (void *)stub_success;
  p->bq_vtable[0] = (void *)bq_Enqueue;
  p->bq_vtable[1] = (void *)bq_Clear;
  p->bq_vtable[2] = (void *)bq_GetState_or_RegisterCallback;
  p->bq_vtable[3] = (void *)bq_RegisterCallback;
  p->bq_ptr = p->bq_vtable;

  for (int i = 0; i < 8; i++) p->effectsend_vtable[i] = (void *)stub_success;
  p->effectsend_ptr = p->effectsend_vtable;
}

/* Output mix */
static void *g_outmix_vtable[8];
static void *g_outmix_ptr;

static SLresult outmix_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult outmix_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self; (void)iid;
  static void *stub_vtable[8];
  static void *stub_ptr;
  static int inited = 0;
  if (!inited) {
    for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
    stub_ptr = stub_vtable;
    inited = 1;
  }
  if (pInterface) *pInterface = &stub_ptr;
  return SL_RESULT_SUCCESS;
}

static void outmix_Destroy(void *self) { (void)self; }

static void init_outmix(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;
  for (int i = 0; i < 8; i++) g_outmix_vtable[i] = (void *)stub_success;
  g_outmix_vtable[0] = (void *)outmix_Realize;
  g_outmix_vtable[2] = (void *)obj_GetState;
  g_outmix_vtable[3] = (void *)outmix_GetInterface;
  g_outmix_vtable[6] = (void *)outmix_Destroy;
  g_outmix_ptr = g_outmix_vtable;
}

/* Engine interface */
static SLresult engine_CreateOutputMix(void *self, void **pMix,
                                        SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds,
                                        const SLBoolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  fprintf(stderr, "[SL] CreateOutputMix\n");
  init_outmix();
  if (pMix) *pMix = &g_outmix_ptr;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                          void *pAudioSrc, void *pAudioSnk,
                                          SLuint32 numInterfaces,
                                          const SLInterfaceID *pInterfaceIds,
                                          const SLBoolean *pInterfaceRequired) {
  (void)self; (void)pAudioSnk; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  fprintf(stderr, "[SL] CreateAudioPlayer\n");
  ensure_audio_initialized();

  AudioPlayer *p = alloc_player();
  if (!p) {
    debugPrintf("opensles_shim: CreateAudioPlayer FATAL: no player slots\n");
    if (pPlayer) *pPlayer = NULL;
    return SL_RESULT_RESOURCE_ERROR;
  }

  if (pAudioSrc) {
    SLDataSource *src = (SLDataSource *)pAudioSrc;
    if (src->pLocator) {
      SLDataLocator_BufferQueue *loc = (SLDataLocator_BufferQueue *)src->pLocator;
      if (loc->locatorType == SL_DATALOCATOR_BUFFERQUEUE) {
        p->queue_capacity = loc->numBuffers;
      }
    }
    if (src->pFormat) {
      SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
      if (fmt->formatType == SL_DATAFORMAT_PCM) {
        p->num_channels = fmt->numChannels;
        p->sample_rate = fmt->samplesPerSec / 1000;
        p->bits_per_sample = fmt->bitsPerSample;
        fprintf(stderr, "[SL] player fmt: %u ch %u Hz %u bit (q=%u)\n",
                p->num_channels, p->sample_rate, p->bits_per_sample, p->queue_capacity);
      } else {
        fprintf(stderr, "[SL] player formatType=0x%x NAO-PCM (PCM_EX? converter!)\n",
                (unsigned)fmt->formatType);
      }
    }
  }

  if (p->sample_rate == 0) {
    p->num_channels = 2;
    p->sample_rate = 44100;
    p->bits_per_sample = 16;
  }

  setup_player_vtables(p);
  if (pPlayer) *pPlayer = &p->obj_ptr;
  return SL_RESULT_SUCCESS;
}

/* Engine object */
static void *g_engine_obj_vtable[8];
static void *g_engine_obj_ptr;
static void *g_engine_itf_vtable[16];
static void *g_engine_itf_ptr;

static SLresult engine_obj_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult engine_obj_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self;
  if (iid == sl_IID_ENGINE) {
    if (pInterface) *pInterface = &g_engine_itf_ptr;
  } else {
    static void *stub_vtable[8];
    static void *stub_ptr;
    static int inited = 0;
    if (!inited) {
      for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
      stub_ptr = stub_vtable;
      inited = 1;
    }
    if (pInterface) *pInterface = &stub_ptr;
  }
  return SL_RESULT_SUCCESS;
}

static void engine_obj_Destroy(void *self) {
  (void)self;
  if (g_audio_dev) {
    SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_audio_initialized = 0;
  }
}

static void init_engine(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;

  for (int i = 0; i < 8; i++) g_engine_obj_vtable[i] = (void *)stub_success;
  g_engine_obj_vtable[0] = (void *)engine_obj_Realize;
  g_engine_obj_vtable[2] = (void *)obj_GetState;
  g_engine_obj_vtable[3] = (void *)engine_obj_GetInterface;
  g_engine_obj_vtable[6] = (void *)engine_obj_Destroy;
  g_engine_obj_ptr = g_engine_obj_vtable;

  /* s14: stub_success SEM escrever o objeto de saída = ponteiro lixo. O FMOD cria
     AudioRecorder (microfone) no System::init; com lixo, derivava tamanhos
     negativos -> malloc 4.29GB -> fatal "System out of memory (MemoryLabel:
     FMOD)". FEATURE_UNSUPPORTED (12) faz o FMOD desistir LIMPO do subsistema
     (sem microfone — irrelevante p/ o jogo). */
  for (int i = 0; i < 16; i++) g_engine_itf_vtable[i] = (void *)engine_Unsupported;
  g_engine_itf_vtable[2] = (void *)engine_CreateAudioPlayer;
  g_engine_itf_vtable[7] = (void *)engine_CreateOutputMix;
  g_engine_itf_ptr = g_engine_itf_vtable;
}

/* Public API: com a pump thread ativa (s14) o hook do render loop vira no-op
 * (dois pumpers = race nos contadores do ring) */
void opensles_shim_pump_callbacks(void) {
  if (g_pump_thread_on) return;
  pump_callbacks_impl();
}
static void pump_callbacks_impl(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t readable = ring_readable(p);
    uint32_t callback_threshold = p->last_enqueue_size;
    if (callback_threshold == 0 || callback_threshold > (RING_BUFFER_SIZE / 2)) {
      callback_threshold = RING_BUFFER_SIZE / 4;
    }
    /* folga modesta: ~2x o buffer SDL enfileirado, SEM virar moedor de CPU (o ring
     * fundo + max_calls=32 foi o que travou). A robustez real vem do FIX dos draws. */
    uint32_t refill_threshold = callback_threshold * 2;
    uint32_t target = SDL_AUDIO_SAMPLES * 2 /*stereo*/ * 2 /*s16*/ * 2;
    if (refill_threshold < target) refill_threshold = target;
    if (refill_threshold > RING_BUFFER_SIZE / 2) refill_threshold = RING_BUFFER_SIZE / 2;

    int max_calls = 4;
    while (p->callback && readable <= refill_threshold && max_calls > 0) {
      uint32_t counter_before = p->enqueue_counter;
      /* if (p->debug_callback_logs < 16 || counter_before % 64 == 0) {
        debugPrintf("opensles_shim: player %d callback readable=%u threshold=%u counter=%u\n",
                    i, readable, refill_threshold, counter_before);
        p->debug_callback_logs++;
      } */
      { static int n; if (n++ < 8) fprintf(stderr, "[SL] pump: callback p%d #%d (readable=%u thr=%u)\n", i, n, readable, refill_threshold); }
      p->callback(&p->bq_ptr, p->callback_context);

      if (p->ever_enqueued && !p->decoder_done &&
          p->enqueue_counter == counter_before) {
        p->decoder_done = 1;
        /* debugPrintf("opensles_shim: player %d decoder_done after callback readable=%u counter=%u\n",
                    i, ring_readable(p), p->enqueue_counter); */
        break;
      }
      readable = ring_readable(p);
      max_calls--;
    }

    if (!p->callback && p->ever_enqueued && !p->decoder_done &&
        p->queued_count == 0 && readable == 0) {
      p->decoder_done = 1;
      /* debugPrintf("opensles_shim: player %d decoder_done after queue drain\n", i); */
    }

    // HEADATEND: fire play callback when decoder done and ring drained
    if (p->decoder_done && !p->headatend_fired) {
      if (ring_readable(p) == 0) {
        p->headatend_fired = 1;
        if (p->play_callback && (p->play_event_mask & SL_PLAYEVENT_HEADATEND)) {
          p->play_callback(&p->play_ptr, p->play_callback_context, SL_PLAYEVENT_HEADATEND);
          /* debugPrintf("opensles_shim: player %d HEADATEND fired (underruns=%u fadeouts=%u)\n",
                      i, p->underrun_count, p->fadeout_count); */
        }
        if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
        p->play_state = SL_PLAYSTATE_STOPPED;
        queue_reset(p);
        if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      }
    }
  }
}

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  fprintf(stderr, "[SL] slCreateEngine (FMOD entrou no caminho OpenSL)\n");
  init_engine();
  if (pEngine) *pEngine = &g_engine_obj_ptr;
  return SL_RESULT_SUCCESS;
}
