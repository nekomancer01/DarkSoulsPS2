/* Prototypes for tools/native/build-wasm.sh: the C helpers built as Pyodide side modules link these from Pyodide's libc */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int printf(const char *, ...);
#ifdef __cplusplus
}
#endif
