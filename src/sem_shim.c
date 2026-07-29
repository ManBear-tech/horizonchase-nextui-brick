/*
 * sem_shim.c — semáforos POSIX próprios p/ so-loader bionic→glibc.
 *
 * CAUSA-RAIZ do deadlock no boot do Cuphead (Unity 2017 IL2CPP): a thread pool
 * do Unity (UnityPreload/Workers) espera em sem_wait; a main acorda via sem_post.
 * Mas o `sem_t` do bionic (Android) tem 4 bytes e o do glibc 32 bytes. O Unity
 * embute o sem_t no tamanho bionic; os sem_* do glibc (resolvidos via PLT) operam
 * em 32 bytes → corrompem memória adjacente e o contador nunca funciona →
 * sem_post NÃO acorda sem_wait → a thread de preload nunca roda → a main fica
 * presa esperando a operação async completar (frame ~110).
 *
 * FIX (mesma ideia do my_sigaction bionic/glibc): interceptar sem_* e implementar
 * com pthread mutex+cond próprios, indexados pelo PONTEIRO do sem (handle opaco) —
 * o layout do sem_t do Unity passa a ser irrelevante.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <signal.h>
static int stid(void) { return (int)syscall(SYS_gettid); }
/* GC-SAFE WAIT (def. em pthread_fake.c): desbloqueia SIGPWR/SIGXCPU em volta do wait real
   p/ o stop-the-world do GC conseguir suspender a thread enquanto ela está bloqueada aqui. */
extern void gc_wait_unblock(void *oldp);
extern void gc_wait_restore(void *oldp);

#define MAX_SEMS 8192   /* 512 estourava no carregamento do título (Unity "Failed to post
                           to a semaphore"=nosso sh_sem_post retornava -1=sem slot livre) */
struct mysem { void *key; pthread_mutex_t m; pthread_cond_t c; int count; int used;
               int wtid[6]; int nwt; };  /* tids distintos que já esperaram (p/ filtro de tick) */
static struct mysem g_sems[MAX_SEMS];
static pthread_mutex_t g_sems_lock = PTHREAD_MUTEX_INITIALIZER;

static struct mysem *sem_lookup(void *s, int create, unsigned initval) {
  pthread_mutex_lock(&g_sems_lock);
  struct mysem *r = NULL, *freeslot = NULL;
  for (int i = 0; i < MAX_SEMS; i++) {
    if (g_sems[i].used && g_sems[i].key == s) { r = &g_sems[i]; break; }
    if (!g_sems[i].used && !freeslot) freeslot = &g_sems[i];
  }
  if (!r && create && freeslot) {
    r = freeslot;
    r->used = 1; r->key = s; r->count = (int)initval;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
  }
  pthread_mutex_unlock(&g_sems_lock);
  return r;
}

static int g_n_init, g_n_post, g_n_wait;
/* TESTE (CUP_PRELOAD_TICK): a UnityPreload thread dorme em sem_wait esperando ser
   acordada p/ processar um item da fila ([+256]=1 no PreloadManager), mas o post
   nunca chega -> a main fica presa esperando a fila esvaziar. Capturamos o(s) sem(s)
   em que threads NÃO-main bloqueiam e os postamos periodicamente p/ dar "ticks". */
int g_main_tid = 0;
static unsigned long g_ubase, g_ibase, g_usize, g_isize;  /* bases p/ resolver caller offsets */
static void *g_tick_sems[8]; static int g_n_tick = 0;
static void register_tick_sem(void *s) {
  for (int i = 0; i < g_n_tick; i++) if (g_tick_sems[i] == s) return;
  if (g_n_tick < 8) g_tick_sems[g_n_tick++] = s;
}
void sh_tick_preload(void) {
  for (int i = 0; i < g_n_tick; i++) {
    struct mysem *m = sem_lookup(g_tick_sems[i], 0, 0);
    if (m) { pthread_mutex_lock(&m->m); m->count++; pthread_cond_signal(&m->c); pthread_mutex_unlock(&m->m); }
  }
}
int sh_sem_init(void *s, int pshared, unsigned value) {
  (void)pshared;
  if (g_n_init++ < 4) fprintf(stderr, "[SEM] init %p val=%u\n", s, value);
  pthread_mutex_lock(&g_sems_lock);
  /* re-init: se já existe, reseta o contador */
  struct mysem *r = NULL, *freeslot = NULL;
  for (int i = 0; i < MAX_SEMS; i++) {
    if (g_sems[i].used && g_sems[i].key == s) { r = &g_sems[i]; break; }
    if (!g_sems[i].used && !freeslot) freeslot = &g_sems[i];
  }
  if (!r && freeslot) {
    r = freeslot; r->used = 1; r->key = s;
    pthread_mutex_init(&r->m, NULL); pthread_cond_init(&r->c, NULL);
  }
  if (r) r->count = (int)value;
  pthread_mutex_unlock(&g_sems_lock);
  return 0;
}

/* CUP_SEMPOLL=ms: sem_wait das threads NÃO-main retorna periodicamente (timeout)
   mesmo sem post — a thread "acorda", checa a fila e re-espera. Contorna a race de
   lost-wakeup do job scheduler do Unity (a main às vezes não faz sem_post ao
   enfileirar). Determinístico, por-thread, sem flood. */
static int g_poll_ms = 0;
void sh_sem_set_poll(int ms) { g_poll_ms = ms; }
int sh_sem_wait(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  if (g_n_wait++ < 60) fprintf(stderr, "[SEM] wait %p tid=%d count=%d\n", s, stid(), m->count);
  int poll = (g_poll_ms > 0 && g_main_tid && stid() != g_main_tid);
  pthread_mutex_lock(&m->m);
  if (m->count <= 0 && g_main_tid && stid() != g_main_tid) register_tick_sem(s);
  while (m->count <= 0) {
    sigset_t o; gc_wait_unblock(&o);
    if (poll) {
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += (long)g_poll_ms * 1000000L;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L; }
      int rc = pthread_cond_timedwait(&m->c, &m->m, &ts);
      gc_wait_restore(&o);
      if (rc == ETIMEDOUT) { pthread_mutex_unlock(&m->m); return 0; } /* polling: retorna sem decrementar */
    } else {
      pthread_cond_wait(&m->c, &m->m);
      gc_wait_restore(&o);
    }
  }
  m->count--;
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_trywait(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  int rc;
  pthread_mutex_lock(&m->m);
  if (m->count > 0) { m->count--; rc = 0; }
  else { errno = EAGAIN; rc = -1; }
  pthread_mutex_unlock(&m->m);
  return rc;
}

int sh_sem_timedwait(void *s, const struct timespec *abs) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  /*
   * Unity occasionally builds an absolute Android deadline by adding a small
   * delta without carrying tv_nsec first. Bionic accepts that path internally,
   * while glibc's pthread_cond_timedwait returns EINVAL for tv_nsec >= 1e9.
   * Passing the raw value made this shim retry EINVAL forever on the main
   * thread. Normalize a private copy and propagate every other condvar error.
   */
  struct timespec normalized;
  const struct timespec *deadline = abs;
  if (abs &&
      (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000L)) {
    long long nanoseconds = abs->tv_nsec;
    normalized = *abs;
    normalized.tv_sec += nanoseconds / 1000000000LL;
    nanoseconds %= 1000000000LL;
    if (nanoseconds < 0) {
      nanoseconds += 1000000000LL;
      normalized.tv_sec--;
    }
    normalized.tv_nsec = (long)nanoseconds;
    deadline = &normalized;
    static unsigned normalized_count;
    if (normalized_count++ < 8)
      fprintf(stderr,
              "[SEM] deadline normalizado: %ld.%09ld -> %ld.%09ld\n",
              (long)abs->tv_sec, abs->tv_nsec,
              (long)normalized.tv_sec, normalized.tv_nsec);
  }
  /* TER_SEMWHO: loga o CALLER (engine) de cada sem_timedwait da MAIN — revela QUAL função
     de libunity/il2cpp está presa no wait do frame 2. Dedup por caller (até 16). */
  if (getenv("TER_SEMWHO") && g_main_tid && stid() == g_main_tid) {
    static unsigned long seen[16]; static int nseen;
    unsigned long ra = (unsigned long)__builtin_return_address(0);
    int known = 0; for (int i = 0; i < nseen; i++) if (seen[i] == ra) { known = 1; break; }
    if (!known && nseen < 16) {
      seen[nseen++] = ra;
      const char *lib = "?"; unsigned long off = ra;
      if (g_ubase && ra >= g_ubase && ra < g_ubase + g_usize) { lib = "libunity"; off = ra - g_ubase; }
      else if (g_ibase && ra >= g_ibase && ra < g_ibase + g_isize) { lib = "libil2cpp"; off = ra - g_ibase; }
      fprintf(stderr, "[SEMWHO] sem_timedwait caller=%s+0x%lx (sem=%p)\n", lib, off, s);
      fsync(2);
    }
  }
  int rc = 0;
  pthread_mutex_lock(&m->m);
  while (m->count <= 0) {
    sigset_t o; gc_wait_unblock(&o);
    if (deadline) {
      rc = pthread_cond_timedwait(&m->c, &m->m, deadline);
      gc_wait_restore(&o);
      if (rc != 0) {
        errno = rc;
        rc = -1;
        break;
      }
    } else {
      pthread_cond_wait(&m->c, &m->m);
      gc_wait_restore(&o);
    }
  }
  if (rc == 0) m->count--;
  pthread_mutex_unlock(&m->m);
  return rc;
}

/* detector de livelock: se a MESMA thread posta o MESMO sem N× seguidas (a main
   girando num loop force-complete), loga o caller (return address) UMA vez por
   marco — revela o loop em libunity/il2cpp que está preso. */
void sh_set_bases(unsigned long ub, unsigned long us, unsigned long ib, unsigned long is) {
  g_ubase = ub; g_usize = us; g_ibase = ib; g_isize = is;
}
/* sinaliza ao watchdog (main.c) que o storm de Signal foi detectado: o ponteiro
 * do sem que está sendo postado em loop + o caller. O watchdog dumpa o estado
 * (singleton/dispatcher) deterministicamente, sem depender do crash flaky. */
volatile int g_sem_storm = 0;
void *volatile g_sem_storm_ptr = 0;
unsigned long volatile g_sem_storm_caller = 0;
int sh_sem_post(void *s) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  if (g_n_post++ < 60) fprintf(stderr, "[SEM] post %p tid=%d\n", s, stid());
  /* livelock spy — SEM I/O (fsync/dump pesado tornava o storm lentíssimo e o jogo
     parecia travado). Loga 1× só, sem fsync, e fast-path drena o storm rápido. */
  static void *last_sem; static int last_tid, streak; static int logged_once;
  int t = stid();
  if (s == last_sem && t == last_tid) {
    streak++;
    if (streak == 150 && !logged_once) {
      logged_once = 1;
      void *ra = __builtin_return_address(0);
      unsigned long r = (unsigned long)ra; const char *lib = "?"; unsigned long off = r;
      if (g_ubase && r >= g_ubase && r < g_ubase + g_usize) { lib = "libunity"; off = r - g_ubase; }
      else if (g_ibase && r >= g_ibase && r < g_ibase + g_isize) { lib = "libil2cpp"; off = r - g_ibase; }
      fprintf(stderr, "[SEM-LIVELOCK] sem=%p tid=%d caller=%s+0x%lx (log 1x, sem fsync)\n", s, t, lib, off);
      if (!g_sem_storm) { g_sem_storm_ptr = s; g_sem_storm_caller = r; g_sem_storm = 1; }
    }
    /* fast-path: drena o storm rápido (só count++, sem cond_signal/I/O) */
    if (streak > 200 && !getenv("CUP_SEMNODRAIN")) {
      pthread_mutex_lock(&m->m); m->count++; pthread_mutex_unlock(&m->m);
      return 0;
    }
  } else { last_sem = s; last_tid = t; streak = 0; }
  pthread_mutex_lock(&m->m);
  m->count++;
  pthread_cond_signal(&m->c);
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_getvalue(void *s, int *sval) {
  struct mysem *m = sem_lookup(s, 1, 0);
  if (!m) return -1;
  pthread_mutex_lock(&m->m);
  if (sval) *sval = m->count;
  pthread_mutex_unlock(&m->m);
  return 0;
}

int sh_sem_destroy(void *s) {
  pthread_mutex_lock(&g_sems_lock);
  for (int i = 0; i < MAX_SEMS; i++)
    if (g_sems[i].used && g_sems[i].key == s) { g_sems[i].used = 0; g_sems[i].key = NULL; break; }
  pthread_mutex_unlock(&g_sems_lock);
  return 0;
}
