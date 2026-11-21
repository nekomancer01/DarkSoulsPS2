/* Prototypes for tools/native/build-wasm.sh: the C helpers built as Pyodide side modules link these from Pyodide's libc */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
#ifdef __cplusplus
}
#endif
