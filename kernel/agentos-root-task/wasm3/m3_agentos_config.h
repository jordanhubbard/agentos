/*
 * agentOS wasm3 configuration overrides
 * 
 * This is included before m3_config.h to set bare-metal options.
 * wasm3 will use its built-in fixed heap allocator.
 */

#ifndef m3_agentos_config_h
#define m3_agentos_config_h

/* Use fixed heap allocator (bump allocator, no malloc needed) */
/* 2MB should be plenty for the WASM runtime + compilation state */
#define d_m3FixedHeap               (2 * 1024 * 1024)
#define d_m3FixedHeapAlign          16

/* Reduce stack height for constrained environment */
#define d_m3MaxFunctionStackHeight  500

/* Limit linear memory to 1MB (64 pages × 64KB) */
#define d_m3MaxLinearMemoryPages    16

/* Keep floats (agents might need math) */
#define d_m3HasFloat                1

/* Disable features we don't need */
#define d_m3VerboseErrorMessages    0
#define d_m3RecordBacktraces        0
#define d_m3EnableOpProfiling       0
#define d_m3EnableOpTracing         0
#define d_m3EnableStrace            0
#define d_m3LogParse                0
#define d_m3LogModule               0
#define d_m3LogCompile              0
#define d_m3LogWasmStack            0
#define d_m3LogEmit                 0
#define d_m3LogCodePages            0
#define d_m3LogRuntime              0
#define d_m3LogNativeStack          0
#define d_m3LogHeapOps              0
#define d_m3LogTimestamps           0

/* Smaller code pages to save memory */
#define d_m3CodePageAlignSize       (8 * 1024)

#endif /* m3_agentos_config_h */
