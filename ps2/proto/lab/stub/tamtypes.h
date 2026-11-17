/* Lab stub (Polaris 1.7b): ps2sdk's integer types for the host build */
#ifndef LAB_TAMTYPES_H
#define LAB_TAMTYPES_H
#include <stdint.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef struct { u32 w[4]; } __attribute__((aligned(16))) u128;  /* (VU / DMA quadwords: only moved, never computed on, in the modules the Lab builds) */
typedef volatile u8 vu8; typedef volatile u16 vu16; typedef volatile u32 vu32; typedef volatile u64 vu64;
#ifndef NULL
#define NULL ((void *)0)
#endif
#endif
