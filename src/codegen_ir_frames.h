// LLVM-IR backend: frame and stack layout (M3).
//
// Maps the C backend's frame and data-stack conventions to LLVM:
//
// * `gs_sp` — the i64 index into the TLS `@gs_stks` array, passed as
//   every stack-using function's first argument (codegen_frames.h:623
//   in the C backend: `int64_t gs_sp`).
// * `gs_at` — an i8* to a `gs_loc { i64 file, i64 line }` struct,
//   the caller's location for a library function's abort message.
// * `gs_stack` — the C `struct { i8* base; i64 top; i64 limit }`,
//   one per thread program, held in `@gs_stks[gs_sp + k]` with
//   per-function constant `k` (the §10.3 hidden-stack-argument
//   strategy).
//
// The TLS globals (gs_gl, gs_stks, gs_nstks, gs_current_thread_id,
// gs_regions, gs_regions_cap) are already emitted in
// codegen_ir_main.h:emit_tls_globals().
//
// This header holds the type/layout helpers; the value-emitting
// mechanics (load_stack, bump, stack_load, stack_store) live in
// codegen_ir_values.h.
#pragma once
#include "codegen_ir_main.h"
#include <vector>

namespace goose {

// The gs_stack layout: { i8* base, i64 top, i64 limit }.
// Mirrors the C struct in src/runtime/runtime.h.
struct IrStackLayout {
    static llvm::StructType *of(llvm::LLVMContext &ctx) {
        auto *i8ptr = llvm::PointerType::get(ctx, 0);
        auto *i64   = llvm::Type::getInt64Ty(ctx);
        return llvm::StructType::get(ctx, { i8ptr, i64, i64 });
    }
};

// The gs_loc layout: { i64 file_idx, i64 line }. A library function's
// abort reports the caller's location through this (cf. C backend's
// `const gs_loc *gs_at`, codegen.h:219-220).
struct IrLocLayout {
    static llvm::StructType *of(llvm::LLVMContext &ctx) {
        auto *i64 = llvm::Type::getInt64Ty(ctx);
        return llvm::StructType::get(ctx, { i64, i64 });
    }
};

// Build the IR function type for a stack-using Goose function:
//   (i64 gs_sp, <params...>) -> <ret>
// where gs_sp is always first. For a function that takes no stacks
// (`needssp == false` in the C backend's SpecInfo), the leading i64
// is omitted. This is the M3 shape; M4 adds the hidden destination
// stack arguments for nonfixed returns and the `gs_at` location.
inline llvm::FunctionType *ir_fn_type(llvm::LLVMContext &ctx,
                                      bool needssp,
                                      llvm::Type *ret,
                                      std::vector<llvm::Type *> params) {
    auto *i64 = llvm::Type::getInt64Ty(ctx);
    std::vector<llvm::Type *> all;
    if (needssp) all.push_back(i64);
    for (auto *p : params) all.push_back(p);
    return llvm::FunctionType::get(ret, all, /*isVarArg=*/false);
}

// The stack index of the k-th data stack owned by the current
// function: gs_sp + k. The C backend spells this `GS(gs_sp + k)`
// (codegen.h:653, `SpIdx`); the IR backend keeps the same constant
// `k` per function (codegen_stacks.h's StackCall.offset).
inline llvm::Value *ir_sp_idx(llvm::IRBuilder<> &b, llvm::Value *gs_sp, int k) {
    if (k == 0) return gs_sp;
    auto *i64 = b.getInt64(k);
    return b.CreateAdd(gs_sp, i64, "sp_idx");
}

} // namespace goose
