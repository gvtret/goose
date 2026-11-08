// LLVM-IR backend: data stack layout and region accounting.
//
// The constant set here described a fictitious three-field
// gs_stack (`{ base, top, limit }`). The runtime's struct is one
// field, `{ top }` (runtime.h:908-910): an element base comes from the
// value's own gs_rhdr { base, len } header, overruns are caught by the
// 1 MiB unmapped guard gap, and the region bases live in the runtime's
// gs_regions[] registry. The `--stack-assert` check lands with the M3
// machinery, against the contract that really exists
// (runtime.h:115-130 for the reservation constants).
//
// Keepers for now, real and unconditionally true:
#pragma once
#include "codegen_ir_frames.h"

namespace goose {

// The gs_loc layout (codegen_ir_frames.h: IrLocLayout), field indices.
constexpr int GS_LOC_FILE = 0;
constexpr int GS_LOC_LINE = 1;

// The runtime's constants this backend passes to gs_rt_start
// (runtime.h:118-130); not emitted into IR as symbols.
constexpr uint64_t GS_STACK_RESERVE_DEFAULT = 2048ull << 20;
constexpr uint64_t GS_STACK_GAP_DEFAULT = 1ull << 20;
constexpr uint64_t GS_STACK_BUDGET_DEFAULT = 32ull << 40;

} // namespace goose
