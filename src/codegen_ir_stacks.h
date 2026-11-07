// LLVM-IR backend: data stack layout and region accounting (M3).
//
// The C backend's §10.3 "hidden stack argument" strategy: every
// stack-using function takes `int64_t gs_sp` as its first argument,
// an index into the TLS `@gs_stks` array. A function's nonfixed
// locals and temporaries live on `gs_stks[gs_sp + k]` with
// per-function constant `k` (codegen_frames.h's SpIdx / NoteStackCall,
// codegen_stacks.h's StackCall.offset).
//
// In the IR backend the same convention holds:
//   * `gs_sp` is an i64 argument (codegen_ir_frames.h: ir_fn_type).
//   * `gs_stks[gs_sp + k]` is loaded with GEP (codegen_ir_values.h:
//     ir_load_stack / ir_sp_idx).
//   * a stack's layout is IrStackLayout::of(ctx) — { i8* base,
//     i64 top, i64 limit }, the C `struct gs_stack` verbatim.
//
// This header holds the layout constants and the region-accounting
// helpers that bound the number of stacks a thread program may use
// (codegen_stacks.h's globalregions / thunkregions / CheckStackLimit,
// M3 ported mechanically).
#pragma once
#include "codegen_ir_frames.h"

namespace goose {

// The fixed layout of a gs_stack, shared with libgoose_rt.a.
// Field indices used by ir_stack_load / ir_stack_store / ir_bump.
constexpr int GS_STACK_BASE = 0;   // i8* base
constexpr int GS_STACK_TOP  = 1;   // i64 top
constexpr int GS_STACK_LIMIT = 2;  // i64 limit

// The gs_loc layout (codegen_ir_frames.h: IrLocLayout), field indices.
constexpr int GS_LOC_FILE = 0;
constexpr int GS_LOC_LINE = 1;

// The region budget per data stack. The C backend's GS_STACK_RESERVE
// default is 2 GiB per region (runtime.h); the IR backend's test
// runner passes `--stack-reserve` through to the emitted IR's
// definition of gs_reserve_region. The guard-gap (a 1 MiB unmapped
// tail) is part of the C runtime's reservation, not the IR module's.
// This constant is only the gap size for the `--stack-assert`
// diagnostic; it is not emitted into IR.
constexpr uint64_t GS_STACK_GAP = 1ull << 20;

// A thread program's maximum data stacks, as the C backend's
// `--maxstacks` caps it (codegen_stacks.h: CheckStackLimit). The IR
// builder does not re-check; it emits the program as-is and the
// runtime's gs_reserve_region fails loudly on overflow.
constexpr int64_t GS_MAX_STACKS_DEFAULT = 0;  // 0 = no limit

} // namespace goose
