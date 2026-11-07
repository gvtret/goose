/* Separate runtime object for the LLVM-IR backend.

   This is the same five-header bundle as the AOT runtime emitted by
   `goose --emit-runtime` (see src/main.cpp:584-597) and the TinyCC host
   runtime in src/jit_runtime.c, but without the hosted-JIT glue. It is
   compiled with Clang into a static archive (libgoose_rt.a) that the
   emitted IR module links against.

   With GS_SEPARATE_RUNTIME defined, GS_API expands to `extern`
   (src/runtime/runtime.h:196-200), so every function definition in
   runtime_impl.h / runtime_threads.h / runtime_os.h becomes an external
   symbol. The TLS state (gs_stks, gs_gl, gs_nstks, gs_current_thread_id,
   gs_regions, gs_regions_cap) and the init thunks (gs_stack_init,
   gs_stack_block, gs_rt_init, gs_thread_spawn) live behind
   #ifndef GS_RUNTIME_OBJECT (src/runtime/runtime.h:1005-1076) and stay
   in the program's TU, as in the current design. The archive defines
   only the GS_API functions; the program TU defines the TLS statics.

   The static helpers inside the archive TU (gs_end, gs_fail, gs_contain,
   gs_tls_*, gs_scan_*, gs_thread_cap, gs_rt_release) remain
   translation-unit-internal, which is correct: they are reached through
   the GS_API functions, not called directly from the IR module.

   See docs/superpowers/specs/2026-11-07-llvm-backend-design.md, M1. */
#define GS_SEPARATE_RUNTIME 1
#define GS_RUNTIME_OBJECT 1
#define GS_NEED_THREADS 1

#include "runtime.h"
#include "runtime_impl.h"
#include "runtime_threads.h"
#include "runtime_ext.h"
#include "runtime_os.h"
