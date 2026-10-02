/* emugcxbox360 - newlib system calls on Linux/PPC32, for running the Xbox 360
   compiler's output under qemu-ppc (tests only). */
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#undef errno
extern int errno;

static long sys(long n, long a, long b, long c, long d, long e, long f) {
  register long r0 __asm__("r0") = n;
  register long r3 __asm__("r3") = a;
  register long r4 __asm__("r4") = b;
  register long r5 __asm__("r5") = c;
  register long r6 __asm__("r6") = d;
  register long r7 __asm__("r7") = e;
  register long r8 __asm__("r8") = f;
  __asm__ volatile("sc\n\tbns+ 1f\n\tneg 3,3\n1:"
                   : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7), "+r"(r8)
                   :
                   : "memory", "cr0", "r9", "r10", "r11", "r12", "ctr", "xer");
  return r3;
}

static int ret(long r) {
  if (r < 0 && r > -4096) {
    errno = (int)-r;
    return -1;
  }
  return (int)r;
}

void _exit(int status) {
  for (;;) sys(234, status, 0, 0, 0, 0, 0);
}
int read(int fd, void* p, size_t n) { return ret(sys(3, fd, (long)p, (long)n, 0, 0, 0)); }
int write(int fd, const void* p, size_t n) { return ret(sys(4, fd, (long)p, (long)n, 0, 0, 0)); }
int close(int fd) { return ret(sys(6, fd, 0, 0, 0, 0, 0)); }
int open(const char* path, int flags, ...) {
  int lf = flags & 3;
  if (flags & O_CREAT) lf |= 0x40;
  if (flags & O_EXCL) lf |= 0x80;
  if (flags & O_TRUNC) lf |= 0x200;
  if (flags & O_APPEND) lf |= 0x400;
  return ret(sys(5, (long)path, lf, 0644, 0, 0, 0));
}
off_t lseek(int fd, off_t off, int whence) { return ret(sys(19, fd, off, whence, 0, 0, 0)); }
int fstat(int fd, struct stat* st) {
  memset(st, 0, sizeof(*st));
  st->st_mode = fd < 3 ? S_IFCHR : S_IFREG;
  st->st_blksize = 65536;
  return 0;
}
int stat(const char* path, struct stat* st) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  close(fd);
  return fstat(3, st);
}
int isatty(int fd) { return fd < 3; }
int getpid(void) { return 1; }
int kill(int pid, int sig) { (void)pid; (void)sig; _exit(128 + sig); return 0; }
int mkdir(const char* p, mode_t m) { return ret(sys(39, (long)p, m, 0, 0, 0, 0)); }
int unlink(const char* p) { return ret(sys(10, (long)p, 0, 0, 0, 0, 0)); }
int gettimeofday(struct timeval* tv, void* tz) { return ret(sys(78, (long)tv, (long)tz, 0, 0, 0, 0)); }

void* sbrk(ptrdiff_t incr) {
  static char* base = 0;
  static size_t used = 0;
  const size_t size = 1u << 30;
  if (!base) {
    long r = sys(90, 0, size, 3, 0x22, -1, 0);  /* mmap anonymous */
    if (r < 0 && r > -4096) { errno = ENOMEM; return (void*)-1; }
    base = (char*)r;
  }
  if (used + incr > size) { errno = ENOMEM; return (void*)-1; }
  char* p = base + used;
  used += incr;
  return p;
}

/* Reentrant variants used by newlib */
struct _reent;
void* _sbrk_r(struct _reent* r, ptrdiff_t i) { (void)r; return sbrk(i); }

int main(int argc, char** argv);
void __libc_init_array(void);
void exit(int);
void _init(void) {}
void _fini(void) {}

/* The toolchain's libm/libstdc++ are built for the Cell/Xenon and contain a
   few 64-bit instructions that qemu-ppc's 32-bit CPUs reject (fcfid, fctid,
   mtocrf/mfocrf). Emulate them from the SIGILL handler. */
struct ppc_mcontext {
  unsigned long gregs[48]; /* r0-r31, nip, msr, orig_r3, ctr, lnk, xer, ccr, ... */
  double fpregs[33];
};
struct kernel_sigaction {
  void (*handler)(int, void*, void*);
  unsigned long flags;
  void (*restorer)(void);
  unsigned long mask[2];
};

static void on_sigill(int sig, void* info, void* uctx) {
  (void)sig;
  (void)info;
  struct ppc_mcontext* mc = *(struct ppc_mcontext**)((char*)uctx + 48);
  unsigned long* g = mc->gregs;
  unsigned inst = *(unsigned*)g[32];
  unsigned op = inst >> 26, xo = (inst >> 1) & 0x3FF;
  unsigned rt = (inst >> 21) & 31, rb = (inst >> 11) & 31;
  if (op == 63 && xo == 846) { /* fcfid */
    unsigned w[2];
    memcpy(w, &mc->fpregs[rb], 8);
    /* (double)(s64) without libgcc (which uses fcfid itself) */
    mc->fpregs[rt] = (double)(int)w[0] * 4294967296.0 + (double)w[1];
  } else if (op == 63 && (xo == 814 || xo == 815)) { /* fctid, fctidz */
    double d = mc->fpregs[rb];
    long long v;
    if (d != d) v = (long long)0x8000000000000000ull;
    else if (d >= 9.2233720368547758e18) v = 0x7FFFFFFFFFFFFFFFll;
    else if (d < -9.2233720368547758e18) v = (long long)0x8000000000000000ull;
    else {
      if (xo == 814) { /* round to nearest even (default FPSCR mode) */
        volatile double big = 4503599627370496.0;
        if (d > -big && d < big) d = d >= 0 ? (d + big) - big : (d - big) + big;
      }
      int neg = d < 0;
      double a = neg ? -d : d;
      unsigned hi = (unsigned)(a / 4294967296.0);
      double rem = a - (double)hi * 4294967296.0;
      unsigned lo = (unsigned)rem;
      unsigned long long u = ((unsigned long long)hi << 32) | lo;
      v = neg ? -(long long)u : (long long)u;
    }
    memcpy(&mc->fpregs[rt], &v, 8);
  } else if (op == 31 && xo == 144) { /* mtcrf / mtocrf */
    unsigned crm = (inst >> 12) & 0xFF, mask = 0;
    for (int i = 0; i < 8; i++)
      if (crm & (0x80 >> i)) mask |= 0xF0000000u >> (4 * i);
    g[38] = (g[38] & ~mask) | (g[rt] & mask);
  } else if (op == 31 && xo == 19) { /* mfcr / mfocrf */
    g[rt] = g[38];
  } else {
    static const char hex[] = "0123456789abcdef";
    char msg[40] = "SIGILL inst ";
    for (int i = 0; i < 8; i++) msg[12 + i] = hex[(inst >> (28 - 4 * i)) & 15];
    msg[20] = ' ';
    for (int i = 0; i < 8; i++) msg[21 + i] = hex[(g[32] >> (28 - 4 * i)) & 15];
    msg[29] = '\n';
    write(2, msg, 30);
    struct kernel_sigaction dfl = {0, 0, 0, {0, 0}};
    sys(173, 4, (long)&dfl, 0, 8, 0, 0);
    return; /* re-executes and dies with the default action */
  }
  g[32] += 4;
}

void shim_start(int argc, char** argv) {
  struct kernel_sigaction sa = {on_sigill, 4 /* SA_SIGINFO */, 0, {0, 0}};
  sys(173, 4 /* SIGILL */, (long)&sa, 0, 8, 0, 0);
  __libc_init_array();
  exit(main(argc, argv));
}
void* __dso_handle = 0;
