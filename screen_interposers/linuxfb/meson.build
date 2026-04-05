cc = meson.get_compiler('c')

# shm_open/shm_unlink live in librt on glibc < 2.17, in libc on newer systems.
librt = cc.find_library('rt', required: false)

# For each *64 function we intercept, check whether it is a distinct linker symbol
# or a macro alias for its non-64 counterpart. On 64-bit glibc, many *64 functions
# are #define'd to their plain counterpart, so providing both definitions would
# produce a duplicate symbol at link time.  The cc.links() probe catches this by
# attempting to define both in one translation unit; failure means they are the same.
_fn64_probes = {
  'open64': '''
    #define _GNU_SOURCE
    #include <fcntl.h>
    #include <stdarg.h>
    int open(const char *p, int f, ...) { (void)p; (void)f; return -1; }
    int open64(const char *p, int f, ...) { (void)p; (void)f; return -1; }
    int main(void) { return 0; }
  ''',
  'openat64': '''
    #define _GNU_SOURCE
    #include <fcntl.h>
    #include <stdarg.h>
    int openat(int d, const char *p, int f, ...) { (void)d; (void)p; (void)f; return -1; }
    int openat64(int d, const char *p, int f, ...) { (void)d; (void)p; (void)f; return -1; }
    int main(void) { return 0; }
  ''',
  'stat64': '''
    #define _GNU_SOURCE
    #include <sys/stat.h>
    int stat(const char *p, struct stat *s) { (void)p; (void)s; return -1; }
    int stat64(const char *p, struct stat64 *s) { (void)p; (void)s; return -1; }
    int main(void) { return 0; }
  ''',
  'lstat64': '''
    #define _GNU_SOURCE
    #include <sys/stat.h>
    int lstat(const char *p, struct stat *s) { (void)p; (void)s; return -1; }
    int lstat64(const char *p, struct stat64 *s) { (void)p; (void)s; return -1; }
    int main(void) { return 0; }
  ''',
  'fstat64': '''
    #define _GNU_SOURCE
    #include <sys/stat.h>
    int fstat(int fd, struct stat *s) { (void)fd; (void)s; return -1; }
    int fstat64(int fd, struct stat64 *s) { (void)fd; (void)s; return -1; }
    int main(void) { return 0; }
  ''',
  'mmap64': '''
    #define _GNU_SOURCE
    #include <sys/mman.h>
    void *mmap(void *a, size_t l, int p, int f, int fd, long o) { return (void*)0; }
    void *mmap64(void *a, size_t l, int p, int f, int fd, long long o) { return (void*)0; }
    int main(void) { return 0; }
  ''',
}

_interposer_args = ['-D_GNU_SOURCE']
foreach _fn, _snippet : _fn64_probes
  if cc.links(_snippet, name: _fn + ' is a distinct symbol')
    _interposer_args += '-DHAVE_DISTINCT_' + _fn.to_upper()
  endif
endforeach

linuxfb_headers_dep = declare_dependency(
  include_directories: include_directories('.'),
)

shared_library('linuxfb_interposer',
  'linuxfb_interposer.c',
  c_args: _interposer_args,
  dependencies: [
    dependency('dl'),
    librt,
  ],
  install: false,
)
