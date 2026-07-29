/* pthread_fake.c — bridge pthread bionic(armhf) -> glibc, p/ o so-loader.
 *
 * Problema de ABI (ARM 32-bit): o bionic guarda mutex/cond/sem em 4 bytes,
 * mas a glibc usa 24/48/16 bytes. Se a glibc escrever no slot de 4 bytes do
 * jogo, corrompe memória. Solução: o slot de 4 bytes do jogo guarda um
 * PONTEIRO p/ um objeto glibc que a gente aloca (lazy-init cobre os
 * PTHREAD_*_INITIALIZER estáticos, que vêm zerados).
 *
 * pthread_t / pthread_key_t / pthread_once_t são 4 bytes em ambos no armhf,
 * então pthread_create/key/once mapeiam ~direto (attr ignorado).
 *
 * Baseado na lógica do gtactw_vita (que usa primitivas Vita); aqui = glibc.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* lazy-init RACE-SAFE: o slot (4/8B do primitivo do jogo) guarda um ponteiro p/ o
 * objeto glibc. Se 2 threads tocam um primitivo ESTÁTICO (PTHREAD_*_INITIALIZER=zerado)
 * ao mesmo tempo, cada uma criava o SEU objeto e a 2ª sobrescrevia a 1ª → uma thread
 * espera no cond C1 enquanto a outra sinaliza C2 → LOST WAKEUP (a causa-raiz dos
 * travamentos do job-system entre frames). Fix: lock GLOBAL só no caminho de CRIAÇÃO
 * (serializa, impede double-create). Fast-path = leitura simples do slot (SEM atômico —
 * os slots do bionic podem ser 4-byte-aligned e um LDAR de 8B faultaria SIGBUS). O lock
 * só é disputado no 1º toque de cada primitivo; depois, zero contenção. */
static pthread_mutex_t g_lazy_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *mtx_get(void **slot) {
  void *cur = *slot;                         /* fast-path: leitura simples (sem atômico) */
  if (cur) return (pthread_mutex_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    *slot = cur = m;
  }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_mutex_t *)cur;
}
int pthread_mutex_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; mtx_get(slot); return 0;
}
int pthread_mutex_destroy_fake(void **slot) {
  if (*slot) { pthread_mutex_destroy((pthread_mutex_t *)*slot); free(*slot); *slot = NULL; }
  return 0;
}
int pthread_mutex_lock_fake(void **slot) { return pthread_mutex_lock(mtx_get(slot)); }
int pthread_mutex_unlock_fake(void **slot) {
  if (!*slot) return 0; return pthread_mutex_unlock((pthread_mutex_t *)*slot);
}
int pthread_mutex_trylock_fake(void **slot) { return pthread_mutex_trylock(mtx_get(slot)); }

/* ===== TER_CONDTRACE: descobre se o cond em que a MAIN espera é ALGUMA VEZ sinalizado.
   A main registra o cslot em que dorme; qualquer signal/broadcast NESSE cslot é logado
   alto (com tid+caller). Se NUNCA aparecer -> o produtor nunca roda. */
#include <sys/syscall.h>
#include <unistd.h>
static unsigned long g_pt_ub, g_pt_ib;
void pt_set_bases(unsigned long ub, unsigned long ib) { g_pt_ub = ub; g_pt_ib = ib; }
static volatile void *g_main_wait_cslot;
static int g_condtrace = -1;
static int condtrace_on(void) { if (g_condtrace < 0) g_condtrace = getenv("TER_CONDTRACE") ? 1 : 0; return g_condtrace; }
static const char *ra_name(unsigned long ra, unsigned long *po) {
  if (g_pt_ub && ra >= g_pt_ub && ra < g_pt_ub + 0x2000000) { *po = ra - g_pt_ub; return "libunity"; }
  if (g_pt_ib && ra >= g_pt_ib && ra < g_pt_ib + 0x3000000) { *po = ra - g_pt_ib; return "libil2cpp"; }
  *po = ra; return "?";
}
static void ct_signal(void **slot, int bcast) {
  if (!condtrace_on() || !g_main_wait_cslot || slot != g_main_wait_cslot) return;
  unsigned long o; const char *l = ra_name((unsigned long)__builtin_return_address(1), &o);
  fprintf(stderr, "[CT] *** %s p/ cslot=%p da MAIN por tid=%d caller=%s+0x%lx ***\n",
          bcast ? "BROADCAST" : "SIGNAL", (void *)slot, (int)syscall(SYS_gettid), l, o);
  fsync(2);
}

/* ---------- cond ---------- */
static pthread_cond_t *cond_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_cond_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { pthread_cond_t *c = malloc(sizeof(pthread_cond_t)); pthread_cond_init(c, NULL); *slot = cur = c; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_cond_t *)cur;
}
int pthread_cond_init_fake(void **slot, const void *attr) {
  (void)attr; *slot = NULL; cond_get(slot); return 0;
}
int pthread_cond_destroy_fake(void **slot) {
  if (*slot) { pthread_cond_destroy((pthread_cond_t *)*slot); free(*slot); *slot = NULL; }
  return 0;
}
int pthread_cond_signal_fake(void **slot) { ct_signal(slot, 0); return pthread_cond_signal(cond_get(slot)); }
int pthread_cond_broadcast_fake(void **slot) { ct_signal(slot, 1); return pthread_cond_broadcast(cond_get(slot)); }
/* CUP_CONDPOLL=ms: defesa contra lost-wakeup em condition variables (mesma classe
   do fix CUP_SEMPOLL p/ semáforos). Em vez de esperar p/ sempre, faz timedwait
   curto e RETORNA (acordada espúria, permitida pelo POSIX) → o chamador re-adquire
   o mutex e re-checa seu predicado no while(); se não pronto, volta a esperar.
   Isso acorda periodicamente a UnityPreload/Workers que ficam presas no futex. */
#include <time.h>
static long g_cond_poll_ms = 0;   /* 0 = wait infinito normal */
void cond_set_poll(int ms) { g_cond_poll_ms = ms; }
/* 🔑 GC-SAFE WAIT: enquanto uma thread está BLOQUEADA num cond/sem do nosso shim, ela está num
   PONTO SEGURO (stack estável, não roda código managed). É exatamente quando o GC PODE suspendê-la.
   As threads do il2cpp (GC Finalizer/Loading) bloqueiam SIGPWR e ficam presas aqui → o stop-the-world
   do GC manda SIGPWR mas é bloqueado → nunca dá ACK → deadlock. FIX: desbloquear SIGPWR/SIGXCPU SÓ
   em volta do wait real → o handler de suspend do GC roda (ack+sigsuspend), o GC manda o restart
   (SIGXCPU), o handler retorna e o cond_wait continua. (≠ desbloquear no thread-create, que expunha
   a thread RODANDO managed → suspendia em ponto ruim → freeze.) g_gc_safe_wait liga (default ON). */
/* default OFF: as threads do GC são bionic-static e bloqueiam SIGPWR via syscall inline,
   bypassando nossos shims — desbloquear no nosso wait NÃO as alcança. Liga via TER_GCSAFEWAIT. */
int g_gc_safe_wait = 0;
void set_gc_safe_wait(int on) { g_gc_safe_wait = on; }
__attribute__((constructor)) static void gc_safe_wait_env(void) { if (getenv("TER_GCSAFEWAIT")) g_gc_safe_wait = 1; }
void gc_wait_unblock(void *oldp) {   /* oldp = sigset_t* */
  if (!g_gc_safe_wait) return;
  sigset_t un; sigemptyset(&un); sigaddset(&un, SIGPWR); sigaddset(&un, SIGXCPU);
  pthread_sigmask(SIG_UNBLOCK, &un, (sigset_t *)oldp);
}
void gc_wait_restore(void *oldp) {
  if (!g_gc_safe_wait) return;
  pthread_sigmask(SIG_SETMASK, (sigset_t *)oldp, NULL);
}
static void condwho_log(void) {
  static unsigned long seen[24]; static int ns;
  unsigned long ra = (unsigned long)__builtin_return_address(1);
  for (int i = 0; i < ns; i++) if (seen[i] == ra) return;
  if (ns >= 24) return; seen[ns++] = ra;
  const char *l = "?"; unsigned long o = ra;
  if (g_pt_ub && ra >= g_pt_ub && ra < g_pt_ub + 0x2000000) { l = "libunity"; o = ra - g_pt_ub; }
  else if (g_pt_ib && ra >= g_pt_ib && ra < g_pt_ib + 0x3000000) { l = "libil2cpp"; o = ra - g_pt_ib; }
  fprintf(stderr, "[CONDWHO] cond_wait caller=%s+0x%lx\n", l, o); fsync(2);
}
int pthread_cond_wait_fake(void **cslot, void **mslot) {
  if (getenv("TER_CONDWHO")) condwho_log();
  if (condtrace_on()) {
    int tid = (int)syscall(SYS_gettid);
    if (tid == getpid()) g_main_wait_cslot = cslot;
    /* dedupe por (tid,cslot) p/ não floodar; loga o grafo de espera */
    static struct { int tid; void *cs; } seen[128]; static int ns;
    int dup = 0; for (int i = 0; i < ns; i++) if (seen[i].tid == tid && seen[i].cs == cslot) { dup = 1; break; }
    if (!dup) {
      if (ns < 128) { seen[ns].tid = tid; seen[ns].cs = cslot; ns++; }
      char comm[20] = ""; FILE *f = fopen("/proc/self/comm", "r"); if (f) { if (fgets(comm, sizeof comm, f)) { char *nl = strchr(comm, '\n'); if (nl) *nl = 0; } fclose(f); }
      unsigned long o; const char *l = ra_name((unsigned long)__builtin_return_address(0), &o);
      fprintf(stderr, "[CT] WAIT tid=%d(%s) cslot=%p caller=%s+0x%lx%s\n",
              tid, comm, (void *)cslot, l, o, tid == getpid() ? " <<MAIN" : ""); fsync(2);
    }
  }
  if (g_cond_poll_ms <= 0) {
    sigset_t old; gc_wait_unblock(&old);
    int r = pthread_cond_wait(cond_get(cslot), mtx_get(mslot));
    gc_wait_restore(&old);
    return r;
  }
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += g_cond_poll_ms * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
  return (r == ETIMEDOUT) ? 0 : r;   /* timeout vira wakeup espúrio */
}
int pthread_cond_timedwait_fake(void **cslot, void **mslot, const struct timespec *ts) {
  sigset_t old; gc_wait_unblock(&old);
  int r = pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), ts);
  gc_wait_restore(&old);
  return r;
}

/* ---------- sem ---------- */
static sem_t *sem_get(void **slot) {
  void *cur = *slot;
  if (cur) return (sem_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, 0); *slot = cur = s; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (sem_t *)cur;
}
int sem_init_fake(void **slot, int pshared, unsigned value) {
  (void)pshared; sem_t *s = malloc(sizeof(sem_t)); sem_init(s, 0, value); *slot = s; return 0;
}
int sem_destroy_fake(void **slot) {
  if (*slot) { sem_destroy((sem_t *)*slot); free(*slot); *slot = NULL; } return 0;
}
int sem_post_fake(void **slot) { return sem_post(sem_get(slot)); }
int sem_wait_fake(void **slot) { return sem_wait(sem_get(slot)); }
int sem_trywait_fake(void **slot) { return sem_trywait(sem_get(slot)); }

/* ---------- create / attr (mapeiam ~direto; attr bionic ignorado) ---------- */
/* trampolim: instala um sigaltstack PRÓPRIO nesta thread antes de chamar o start
 * real. Sem isso, um SIGSEGV numa thread do Unity roda o on_crash na pilha da
 * própria thread (que pode estar corrompida/cheia) → re-fault → morte calada.
 * Com altstack por-thread, o on_crash sempre consegue dumpar. */
#include <signal.h>
#include <sys/mman.h>
/* registro de threads (pthread_t -> comm) p/ identificar quem o GC sinaliza no [PKILL]. */
#include <sys/syscall.h>
#define TER_MAXTHR 96
static struct { pthread_t t; int tid; char comm[20]; } g_thrreg[TER_MAXTHR];
static int g_thrreg_n;
static pthread_mutex_t g_thrreg_lock = PTHREAD_MUTEX_INITIALIZER;
static void ter_thread_reg(void) {
  pthread_mutex_lock(&g_thrreg_lock);
  if (g_thrreg_n < TER_MAXTHR) {
    int i = g_thrreg_n++;
    g_thrreg[i].t = pthread_self();
    g_thrreg[i].tid = (int)syscall(SYS_gettid);
    char pth[64]; snprintf(pth, sizeof pth, "/proc/self/task/%d/comm", g_thrreg[i].tid);
    FILE *f = fopen(pth, "r");
    if (f) { if (fgets(g_thrreg[i].comm, sizeof g_thrreg[i].comm, f)) { char *nl = strchr(g_thrreg[i].comm, '\n'); if (nl) *nl = 0; } fclose(f); }
  }
  pthread_mutex_unlock(&g_thrreg_lock);
}
const char *ter_thread_comm(pthread_t t) {
  static char buf[24];
  for (int i = 0; i < g_thrreg_n; i++) if (pthread_equal(g_thrreg[i].t, t)) {
    char pth[64]; snprintf(pth, sizeof pth, "/proc/self/task/%d/comm", g_thrreg[i].tid);
    FILE *f = fopen(pth, "r");   /* lê o comm ATUAL (Unity renomeia depois do start) */
    if (f) { buf[0] = 0; if (fgets(buf, sizeof buf, f)) { char *nl = strchr(buf, '\n'); if (nl) *nl = 0; } fclose(f);
             snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "/%d", g_thrreg[i].tid); return buf; }
    return g_thrreg[i].comm;
  }
  return "?(unreg)";
}
struct thr_boot { void *(*start)(void *); void *arg; };
static void *thr_trampoline(void *p) {
  struct thr_boot b = *(struct thr_boot *)p;
  free(p);
  ter_thread_reg();   /* registra pthread_self()->comm assim que a thread sobe */
  /* TER_JOBLOG: loga start_routine (offset libunity) + arg + deref do contexto p/ cada thread
     criada pela engine. Correlacionado com TER_FUTEXLOG (uaddr da wait), identifica o ponteiro
     do JobQueue/contexto de CADA worker → resolve "1 ou 2 instâncias de JobQueue" do handoff. */
  if (getenv("TER_JOBLOG")) {
    extern uintptr_t ter_unity_base(void);
    uintptr_t ub = ter_unity_base();
    uintptr_t so = (uintptr_t)b.start;
    int tid = (int)syscall(178 /*arm64 gettid*/);
    char comm[20] = ""; FILE *f = fopen("/proc/self/comm", "r");
    if (f) { if (fgets(comm, sizeof comm, f)) { char *nl = strchr(comm, '\n'); if (nl) *nl = 0; } fclose(f); }
    char line[512]; int n = 0;
    n += snprintf(line + n, sizeof line - n, "[JOBTHR] tid=%d(%s) start=libunity+0x%lx arg=%p",
                  tid, comm, ub ? so - ub : so, b.arg);
    if (b.arg) { void **a = (void **)b.arg;
      for (int i = 0; i < 6 && n < (int)sizeof line - 32; i++) {
        uintptr_t v = (uintptr_t)a[i];
        n += snprintf(line + n, sizeof line - n, " a[%d]=%p%s", i, a[i],
                      (ub && v >= ub && v < ub + 0x1000000) ? "(U)" : ""); } }
    snprintf(line + n, sizeof line - n, "\n");
    fputs(line, stderr); fsync(2);
  }
  size_t asz = 256 * 1024;   /* fixo e generoso (SIGSTKSZ pode ser minúsculo/dinâmico
                                e o on_crash dumpa bastante → estouraria) */
  void *mem = mmap(NULL, asz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem != MAP_FAILED) {
    stack_t ss; ss.ss_sp = mem; ss.ss_size = asz; ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
  }
  /* OPT-IN TER_SIGUNBLK: desbloqueia SIGPWR/SIGXCPU nesta thread. NÃO usar por padrão — as
     threads do il2cpp bloqueiam SIGPWR de PROPÓSITO (suspensão COOPERATIVA via safepoint);
     forçar o sinal faz algumas suspenderem sem restart → freeze (regrediu p/ frame 1). */
  if (getenv("TER_SIGUNBLK")) {
    sigset_t un; sigemptyset(&un); sigaddset(&un, SIGPWR); sigaddset(&un, SIGXCPU);
    pthread_sigmask(SIG_UNBLOCK, &un, NULL);
  }
  return b.start(b.arg);
}
int pthread_create_fake(pthread_t *t, const void *attr, void *(*start)(void *), void *arg) {
  (void)attr;
  struct thr_boot *b = malloc(sizeof *b);
  if (!b) return pthread_create(t, NULL, start, arg);
  b->start = start; b->arg = arg;
  int rc = pthread_create(t, NULL, thr_trampoline, b);
  if (rc) { free(b); rc = pthread_create(t, NULL, start, arg); }
  return rc;
}
int pthread_attr_init_fake(void *a) { (void)a; return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { (void)a; (void)s; return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { (void)a; (void)s; return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }
int pthread_setschedparam_fake(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
int pthread_setname_np_fake(pthread_t t, const char *n) { (void)t; (void)n; return 0; }

/* bionic: pthread_cond_timeout_np(cond, mutex, ms) — espera relativa em ms */
#include <time.h>
int pthread_cond_timeout_np_fake(void **cslot, void **mslot, unsigned ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  return pthread_cond_timedwait(cond_get(cslot), mtx_get(mslot), &ts);
}

/* ======== GAP arm64: rwlock/once/key/thread-misc/sem-extras ======== */
#include <signal.h>
#include <sched.h>
#include <time.h>
/* ---------- rwlock (slot->ptr glibc, igual mutex) ---------- */
static pthread_rwlock_t *rwl_get(void **slot) {
  void *cur = *slot;
  if (cur) return (pthread_rwlock_t *)cur;
  pthread_mutex_lock(&g_lazy_lock);
  cur = *slot;
  if (!cur) { pthread_rwlock_t *r = malloc(sizeof(pthread_rwlock_t)); pthread_rwlock_init(r, NULL); *slot = cur = r; }
  pthread_mutex_unlock(&g_lazy_lock);
  return (pthread_rwlock_t *)cur;
}
int pthread_rwlock_init_fake(void **slot, const void *a) { (void)a; *slot = NULL; rwl_get(slot); return 0; }
int pthread_rwlock_destroy_fake(void **slot) { if (*slot) { pthread_rwlock_destroy((pthread_rwlock_t *)*slot); free(*slot); *slot = NULL; } return 0; }
int pthread_rwlock_rdlock_fake(void **slot) { return pthread_rwlock_rdlock(rwl_get(slot)); }
int pthread_rwlock_wrlock_fake(void **slot) { return pthread_rwlock_wrlock(rwl_get(slot)); }
int pthread_rwlock_tryrdlock_fake(void **slot) { return pthread_rwlock_tryrdlock(rwl_get(slot)); }
int pthread_rwlock_trywrlock_fake(void **slot) { return pthread_rwlock_trywrlock(rwl_get(slot)); }
int pthread_rwlock_unlock_fake(void **slot) { if (!*slot) return 0; return pthread_rwlock_unlock((pthread_rwlock_t *)*slot); }
/* ---------- thread misc (forward direto; pthread_t = 8B arm64 compat) ---------- */
int pthread_detach_fake(pthread_t t) { return pthread_detach(t); }
int pthread_join_fake(pthread_t t, void **r) { return pthread_join(t, r); }
pthread_t pthread_self_fake(void) { return pthread_self(); }
int pthread_getschedparam_fake(pthread_t t, int *pol, void *par) { return pthread_getschedparam(t, pol, (struct sched_param *)par); }
/* 🔑 pthread_sigmask: ABI bionic≠glibc. bionic sigset_t arm64 = 8 BYTES (máscara 64-bit);
   glibc sigset_t = 128 BYTES. O passthrough antigo passava o set bionic (8B) direto pro glibc
   → glibc LIA 128B (120B de lixo da stack → bloqueava sinais ALEATÓRIOS, incl. SIGPWR que o GC
   usa p/ suspender threads) e ESCREVIA 128B no `old` de 8B (corrupção). Resultado: uma thread
   ficava com SIGPWR mascarado → nunca recebia o suspend do GC → nunca dava ACK → o stop-the-world
   (WaitForThreadsToSuspend, libil2cpp+0x74f260) TRAVAVA p/ sempre (frame 2). FIX: traduzir
   bionic(64-bit) <-> glibc sigset_t corretamente. */
int pthread_sigmask_fake(int how, const void *set, void *old) {
  sigset_t gset, gold; sigset_t *pset = NULL, *pold = NULL;
  if (getenv("TER_SIGMASKLOG")) { static int n; if (n++ < 12)
    fprintf(stderr, "[SIGMASK] how=%d set=%s setmask=0x%lx\n", how, set?"y":"n",
            set ? *(const unsigned long *)set : 0UL); }
  if (set) {
    sigemptyset(&gset);
    unsigned long bm = *(const unsigned long *)set;   /* bionic: bit (s-1) = sinal s */
    /* OPT-IN TER_SIGFILTER (default OFF — ligar REGREDIA p/ frame 1): o il2cpp BLOQUEIA
       SIGPWR/SIGXCPU de PROPÓSITO na thread coordenadora do GC (ela não deve ser suspensa).
       Filtrar o block faz ela receber SIGPWR e suspender errado → freeze. Manter o block. */
    if (getenv("TER_SIGFILTER") && (how == SIG_BLOCK || how == SIG_SETMASK)) {
      bm &= ~(1UL << (30 - 1));   /* SIGPWR */
      bm &= ~(1UL << (24 - 1));   /* SIGXCPU */
    }
    for (int s = 1; s <= 64; s++) if (bm & (1UL << (s - 1))) sigaddset(&gset, s);
    pset = &gset;
  }
  if (old) pold = &gold;
  int r = pthread_sigmask(how, pset, pold);
  if (old) {
    unsigned long bm = 0;
    for (int s = 1; s <= 64; s++) if (sigismember(&gold, s)) bm |= (1UL << (s - 1));
    *(unsigned long *)old = bm;                        /* devolve máscara bionic de 8B */
  }
  return r;
}
/* ---------- key / specific (pthread_key_t compat) ---------- */
int pthread_key_create_fake(unsigned *k, void (*d)(void *)) { return pthread_key_create((pthread_key_t *)k, d); }
int pthread_key_delete_fake(unsigned k) { return pthread_key_delete((pthread_key_t)k); }
void *pthread_getspecific_fake(unsigned k) { return pthread_getspecific((pthread_key_t)k); }
int pthread_setspecific_fake(unsigned k, const void *v) { return pthread_setspecific((pthread_key_t)k, v); }
/* ---------- once (bionic once_t = int) ---------- */
int pthread_once_fake(int *o, void (*f)(void)) { return pthread_once((pthread_once_t *)o, f); }
/* ---------- mutexattr (no-op; mtx_get usa RECURSIVE) ---------- */
int pthread_mutexattr_init_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_mutexattr_settype_fake(void *a, int t) { (void)a; (void)t; return 0; }
/* ---------- sem extras ---------- */
int sem_getvalue_fake(void **slot, int *v) { return sem_getvalue(sem_get(slot), v); }
int sem_timedwait_fake(void **slot, const struct timespec *ts) { return sem_timedwait(sem_get(slot), ts); }

/* ======== GAP Cuphead (Unity IL2CPP): condattr/attr_getstack/getattr_np ======== */
int pthread_condattr_init_fake(void *a){ (void)a; return 0; }
int pthread_condattr_destroy_fake(void *a){ (void)a; return 0; }
int pthread_condattr_setclock_fake(void *a, int clk){ (void)a; (void)clk; return 0; }
/* Unity usa getattr_np+attr_getstack p/ achar os limites da stack (GC scan).
 * F0: devolve uma faixa plausível (8MB abaixo do SP atual). */
int pthread_getattr_np_fake(pthread_t t, void *attr){ (void)t; (void)attr; return 0; }
int pthread_attr_getstack_fake(void *attr, void **addr, size_t *size){ (void)attr;
  unsigned long sp; __asm__ volatile("mov %0, sp":"=r"(sp));
  size_t sz = 8*1024*1024; unsigned long base = (sp & ~0xfffUL) - sz;
  if(addr) *addr = (void*)base; if(size) *size = sz; return 0; }
