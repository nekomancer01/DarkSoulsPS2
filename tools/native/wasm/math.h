/* Prototypes for tools/native/build-wasm.sh: the C helpers built as Pyodide side modules link these from Pyodide's libc */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double sqrt(double); double fabs(double); double floor(double); double fmin(double, double); double fmax(double, double);
double cos(double); double sin(double); double pow(double, double); double round(double);
float sqrtf(float); float fabsf(float); float floorf(float); float fminf(float, float); float fmaxf(float, float);
float cosf(float); float sinf(float);
#ifdef __cplusplus
}
#endif
#define INFINITY __builtin_inff()
