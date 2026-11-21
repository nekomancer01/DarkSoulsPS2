/* The C++ runtime meshoptimizer needs as a Pyodide side module (tools/native/build-wasm.sh): its default allocator's
 * operator new / delete, on Pyodide's malloc / free (no exceptions: a failed allocation returns null, as malloc) */
#include <stddef.h>
#include <stdlib.h>
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
