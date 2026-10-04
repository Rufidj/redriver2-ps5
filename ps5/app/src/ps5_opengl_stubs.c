/* Local overrides for symbols Mesa/libPS5OpenGL references that either don't
 * exist as real PS5 syscalls (dynamic loading, process spawning, shared
 * memory, syslog - none of these make sense in this project's sandboxed
 * homebrew environment) or fall in the same "links but SIGSEGVs at runtime"
 * class this project has hit before with getenv/opendir (see ps5link-sdk's
 * catalog_extra.c note on those two) - env vars have no real backing in a
 * ps5link title's process, so getenv/setenv/unsetenv get the same safe
 * local-stub treatment here rather than a catalog entry that would resolve
 * to nothing real. Mesa only consults these for optional debug/override
 * behavior (MESA_DEBUG, driconf options, etc.) - "not present" is the
 * correct, safe answer for all of them. */
#include <stddef.h>
#include <stdlib.h>

char *getenv(const char *name) { (void)name; return NULL; }
int setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return 0; }
int unsetenv(const char *name) { (void)name; return 0; }

void *dlopen(const char *file, int mode) { (void)file; (void)mode; return NULL; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return NULL; }
int dladdr(const void *addr, void *info) { (void)addr; (void)info; return 0; }
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return (char *)"dlopen is not available on this platform"; }

void *popen(const char *command, const char *mode) { (void)command; (void)mode; return NULL; }
int pclose(void *stream) { (void)stream; return -1; }
int system(const char *command) { (void)command; return -1; }

void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }
void openlog(const char *ident, int option, int facility) { (void)ident; (void)option; (void)facility; }

int shm_open(const char *name, int flags, unsigned mode) { (void)name; (void)flags; (void)mode; return -1; }

/* Mesa's DRI config-query hooks: with no drirc file on this platform, "not
 * set" (Mesa then falls back to its own built-in default) is always the
 * correct answer - not a missing feature. */
int driQueryOptionb(const void *cache, const char *name) { (void)cache; (void)name; return 0; }
const char *driQueryOptionstr(const void *cache, const char *name) { (void)cache; (void)name; return NULL; }
int driQueryOptioni(const void *cache, const char *name) { (void)cache; (void)name; return 0; }

/* opendir/readdir/closedir/fdopendir: CONFIRMED BROKEN on hardware for this
 * exact class of symbol (see ps5link-sdk's catalog_extra.c and this
 * session's own PS5GL/bgdi work - opendir never returns, SIGSEGV rip=0).
 * Mesa only uses these for optional DRI driver directory scans that do not
 * apply to this static, single-driver build - "nothing found" is safe. */
#include <dirent.h>
DIR *opendir(const char *name) { (void)name; return (DIR *)0; }
DIR *fdopendir(int fd) { (void)fd; return (DIR *)0; }
struct dirent *readdir(DIR *dirp) { (void)dirp; return (struct dirent *)0; }
int closedir(DIR *dirp) { (void)dirp; return 0; }

/* __dso_handle: normally supplied by the compiler's own crtbegin.o for
 * __cxa_atexit-based static destructor registration - this project's
 * custom crt1_ps5.o does not provide one. A plain global is the standard
 * minimal-runtime substitute (used by every libc-less/freestanding target). */
void *__dso_handle = (void *)0;

/* __cxa_thread_atexit_impl: registers a thread_local destructor. The
 * process exits as a whole rather than tearing down individual threads
 * here, so there is nothing meaningful to run this destructor at - safely
 * dropping the registration instead of calling into a real (and more
 * complex) per-thread exit mechanism this platform may not provide anyway. */
int __cxa_thread_atexit_impl(void (*dtor)(void *), void *obj, void *dso_symbol) {
    (void)dtor; (void)obj; (void)dso_symbol;
    return 0;
}

/* __eh_frame_start/__eh_frame_end/__eh_frame_hdr_start/__eh_frame_hdr_end:
 * libunwind's static-binary fallback for finding the .eh_frame section
 * without a dl_iterate_phdr-registered shared library list - normally
 * synthesized by a real linker script wrapping the .eh_frame/.eh_frame_hdr
 * output sections (PROVIDE_HIDDEN(__eh_frame_start = .) etc). This project's
 * scoped-down custom linker (ps5link-sdk/linker) does not collect or lay out
 * those sections at all. Defining an empty (zero-length, same-address)
 * range here means libunwind finds no unwind tables and cannot propagate a
 * real C++ exception through this binary - the Mesa/libc++ code this is
 * linked against is not expected to throw in practice (ps5-opengl's own
 * native-app boilerplate builds its C++20 code with exceptions and RTTI
 * disabled), so this is a safe degraded fallback, not a silent correctness
 * bug: a thrown exception would call std::terminate via the usual
 * no-matching-handler path instead of unwinding, same as -fno-exceptions
 * code hitting one already does. */
/* These must be address markers (plain objects libunwind takes the address
 * of), not pointer variables - the real linker-script form is
 * `extern char __eh_frame_start;` with callers using `&__eh_frame_start`.
 * Aliasing all four to the same one byte gives a true zero-length range for
 * both .eh_frame and .eh_frame_hdr, not just a near-empty one. */
char __eh_frame_start;
extern char __eh_frame_end __attribute__((alias("__eh_frame_start")));
extern char __eh_frame_hdr_start __attribute__((alias("__eh_frame_start")));
extern char __eh_frame_hdr_end __attribute__((alias("__eh_frame_start")));

/* __assert: BSD libc's assertion-failure reporter (noreturn). Mesa's debug
 * builds reference it from util_copy_framebuffer_state et al behind
 * assert() macros that should never actually fire in correct code paths -
 * if one ever does, exiting immediately (same SIGABRT-style exit code a
 * real abort() would produce) is the correct "something is badly wrong,
 * stop now" behavior, just without the real libc's message formatting. */
#include <fcntl.h>
#include <unistd.h>
int snprintf(char *, unsigned long, const char *, ...);
void __assert(const char *func, const char *file, int line, const char *failedexpr) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "ASSERT %s:%d in %s: %s\n", file ? file : "?", line, func ? func : "?", failedexpr ? failedexpr : "?");
    int fd = open("/app0/assert.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { if (n > 0) write(fd, buf, (unsigned long)n); close(fd); }
    (void)func; (void)file; (void)line; (void)failedexpr;
    _Exit(134);
}

/* mkstemps: used only by Mesa's optional log-to-temp-file path
 * (src/util/log.c's mesa_log_init_once) - returning failure makes it fall
 * back to its other logging path instead of creating a temp file, which
 * this platform cannot usefully do anyway (no shared /tmp semantics). */
int mkstemps(char *name_template, int suffix_len) {
    (void)name_template; (void)suffix_len;
    return -1;
}

/* _ZTH23_mesa_glapi_tls_Context: Itanium ABI's auto-generated TLS-init
 * wrapper for Mesa's glapi thread-local Context pointer. Referenced weak
 * from two translation units (main_uniform_query.cpp.o,
 * main_shader_query.cpp.o) with no strongly-defining TU anywhere in this
 * static library - the normal pattern when the TU that actually defines
 * the TLS variable accesses it directly and never needs its own wrapper.
 * A real ld/lld leaves a weak-undefined reference like this as address 0;
 * this project's post-link tool instead requires every referenced symbol to
 * have either a defining object or a public SDK stub, so a real (if inert)
 * local definition satisfies it the same way. Never expected to actually
 * run - if it ever is, returning NULL matches what an unresolved weak
 * symbol would have done anyway. */
void *_ZTH23_mesa_glapi_tls_Context(void) { return (void *)0; }
