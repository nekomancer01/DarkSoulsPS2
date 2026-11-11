/* Prototypes for tools/native/build-wasm.sh: the C helpers built as Pyodide side modules link these from Pyodide's libc */
#pragma once
double sqrt(double); double fabs(double); double floor(double); double fmin(double, double); double fmax(double, double);
