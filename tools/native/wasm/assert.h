/* Prototypes for tools/native/build-wasm.sh: the C helpers built as Pyodide side modules link these from Pyodide's libc */
#pragma once
/* asserts off, as with NDEBUG (they never change what a helper computes) */
#define assert(e) ((void)0)
