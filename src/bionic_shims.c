/* bionic_shims.c — shims bionic p/ o so-loader (F1): FORTIFY _chk -> glibc unchecked,
 * __sF (stdio bionic), __system_property_get, ZSTD trace, android_set_abort_message.
 * Exportados (build c/ -rdynamic) -> o fallback dlsym(RTLD_DEFAULT) do so_resolve acha. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/select.h>

/* ---- FORTIFY _chk: ignoram o arg de tamanho, chamam a versão glibc ---- */
size_t __strlen_chk(const char *s, size_t n){ (void)n; return strlen(s); }
char  *__strchr_chk(const char *s, int c, size_t n){ (void)n; return (char *)strchr(s, c); }
char  *__strcpy_chk(char *d, const char *s, size_t n){ (void)n; return strcpy(d, s); }
char  *__strncpy_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncpy(d, s, n); }
char  *__strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn){ (void)dn; (void)sn; return strncpy(d, s, n); }
char  *__strcat_chk(char *d, const char *s, size_t dn){ (void)dn; return strcat(d, s); }
char  *__strncat_chk(char *d, const char *s, size_t n, size_t dn){ (void)dn; return strncat(d, s, n); }
void  *__memcpy_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memcpy(d, s, n); }
void  *__memmove_chk(void *d, const void *s, size_t n, size_t dn){ (void)dn; return memmove(d, s, n); }
void  *__memset_chk(void *d, int c, size_t n, size_t dn){ (void)dn; return memset(d, c, n); }
ssize_t __write_chk(int fd, const void *b, size_t n, size_t bn){ (void)bn; return write(fd, b, n); }
ssize_t __read_chk(int fd, void *b, size_t n, size_t bn){ (void)bn; return read(fd, b, n); }
ssize_t __sendto_chk(int fd, const void *b, size_t n, size_t bn, int fl, const struct sockaddr *a, socklen_t al){ (void)bn; return sendto(fd, b, n, fl, a, al); }
void __FD_SET_chk(int fd, void *s, size_t n){ (void)n; FD_SET(fd, (fd_set*)s); }
int  __FD_ISSET_chk(int fd, void *s, size_t n){ (void)n; return FD_ISSET(fd, (fd_set*)s); }
void __FD_CLR_chk(int fd, void *s, size_t n){ (void)n; FD_CLR(fd, (fd_set*)s); }
int __vsnprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsnprintf(d, n, f, ap); }
int __snprintf_chk(char *d, size_t n, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsnprintf(d,n,f,ap); va_end(ap); return r; }
int __vsprintf_chk(char *d, int fl, size_t dn, const char *f, va_list ap){ (void)fl; (void)dn; return vsprintf(d, f, ap); }
int __sprintf_chk(char *d, int fl, size_t dn, const char *f, ...){ va_list ap; va_start(ap,f); int r=vsprintf(d,f,ap); va_end(ap); return r; }

/* ---- bionic misc ---- */
int __system_property_get(const char *name, char *value){ (void)name; if(value) value[0]=0; return 0; }
void android_set_abort_message(const char *m){ fprintf(stderr, "[abort] %s\n", m?m:""); }

/* ---- ZSTD trace hooks (opcionais): no-op ---- */
unsigned long long ZSTD_trace_compress_begin(void *cctx){ (void)cctx; return 0; }
void ZSTD_trace_compress_end(unsigned long long id, const void *t){ (void)id; (void)t; }
unsigned long long ZSTD_trace_decompress_begin(void *dctx){ (void)dctx; return 0; }
void ZSTD_trace_decompress_end(unsigned long long id, const void *t){ (void)id; (void)t; }

/* ---- __sF: stdio bionic. F1: buffer válido (3 * tamanho generoso); se o jogo
 * passar &__sF[i] p/ stdio glibc no init, tratamos na F1b. ---- */
char __sF[3 * 512];

/* ====== sigaction/sigprocmask: ABI bionic x glibc (arm64) ======
 * struct sigaction bionic arm64 = { int sa_flags; void* sa_handler; unsigned long sa_mask; void* sa_restorer; } = 32B.
 * glibc = 152B (sigset_t=128B). Se chamar a glibc direto no oldact (buffer bionic de 32B) -> estoura a stack.
 * Campos bsa_* p/ não colidir com as MACROS sa_handler/sa_sigaction da glibc. */
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * bionic arm64 uses one 64-bit word for sigset_t, whereas glibc uses 128
 * bytes. Passing a Unity/IL2CPP sigset_t directly to glibc therefore reads
 * beyond the Android object and, for output functions such as sigemptyset,
 * overwrites 120 bytes of adjacent memory.
 *
 * Keep these entry points separate from the host functions: this file itself
 * must continue to use glibc sigset_t for sigaction/pthread operations.
 */
typedef unsigned long hc_bionic_sigset_t;
_Static_assert(sizeof(hc_bionic_sigset_t) == 8,
               "Horizon Chase requires the arm64 bionic sigset_t ABI");

static void hc_bionic_sigset_to_host(hc_bionic_sigset_t bset,
                                     sigset_t *host) {
  sigemptyset(host);
  for (int signal_number = 1; signal_number <= 64; signal_number++) {
    if (bset & (1UL << (signal_number - 1)))
      sigaddset(host, signal_number);
  }
}

int hc_bionic_sigemptyset(hc_bionic_sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = 0;
  return 0;
}

int hc_bionic_sigfillset(hc_bionic_sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = ~0UL;
  return 0;
}

int hc_bionic_sigaddset(hc_bionic_sigset_t *set, int signal_number) {
  if (!set || signal_number < 1 || signal_number > 64) {
    errno = EINVAL;
    return -1;
  }
  *set |= 1UL << (signal_number - 1);
  return 0;
}

int hc_bionic_sigdelset(hc_bionic_sigset_t *set, int signal_number) {
  if (!set || signal_number < 1 || signal_number > 64) {
    errno = EINVAL;
    return -1;
  }
  *set &= ~(1UL << (signal_number - 1));
  return 0;
}

int hc_bionic_sigsuspend(const hc_bionic_sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  sigset_t host;
  hc_bionic_sigset_to_host(*set, &host);
  return sigsuspend(&host);
}

struct bionic_sigaction { int bsa_flags; void *bsa_handler; unsigned long bsa_mask; void *bsa_restorer; };
int my_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *oldact) {
  struct sigaction ga, go; struct sigaction *pga = NULL, *pgo = NULL;
  /* CUP_NOSIGH: NÃO deixa o engine instalar handler de sinais de crash -> nosso
   * handler pega o fault ORIGINAL (em vez do re-raise do crash handler do Unity). */
  if (getenv("CUP_NOSIGH") && (sig==4||sig==5||sig==6||sig==7||sig==8||sig==11)) { (void)oldact; return 0; }
  if (sig==10) { (void)oldact; return 0; }  /* SIGUSR1 = nosso diag_handler; jogo NÃO sobrescreve */
  /* CUP_GCSIG: não deixa o engine/GC sobrescrever nossos handlers de SIGPWR(30)/
     SIGXCPU(24) — nossas threads usam o protocolo de suspensão que NÃO mata. */
  if (getenv("CUP_GCSIG") && (sig==30||sig==24)) { (void)oldact; return 0; }
  if (act) {
    memset(&ga, 0, sizeof ga); ga.sa_flags = act->bsa_flags;
    if (act->bsa_flags & SA_SIGINFO) ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
    else ga.sa_handler = (void (*)(int))act->bsa_handler;
    sigemptyset(&ga.sa_mask);
    for (int s = 1; s < 64; s++) if (act->bsa_mask & (1UL << (s - 1))) sigaddset(&ga.sa_mask, s);
    pga = &ga;
  }
  if (oldact) { memset(&go, 0, sizeof go); pgo = &go; }
  int r = sigaction(sig, pga, pgo);
  if (oldact) {
    oldact->bsa_flags = go.sa_flags;
    oldact->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
    unsigned long m = 0; for (int s = 1; s < 64; s++) if (sigismember(&go.sa_mask, s)) m |= (1UL << (s - 1));
    oldact->bsa_mask = m; oldact->bsa_restorer = NULL;
  }
  return r;
}
