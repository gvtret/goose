// LLVM-IR backend: value emission primitives (M2).
//
// These are the building blocks that M3 (statement walk) will use to
// emit expressions and constructs. Each function mirrors the C
// backend's TopW / bump / load / store mechanics
// (codegen_frames.h:683, codegen_values.h) in LLVM IRBuilder terms.
//
// The data-stack model (§10.3): every stack-using function takes an
// i64 gs_sp argument (an index into the TLS gs_stks array). Nonfixed
// locals and temporaries live on a data stack, accessed via
// gs_stks[gs_sp + k]. Bump is a store to the stack's top field.
//
// For now these are free functions taking a CodeGenIR&; the full Loc
// / Dst / NrvoDest machinery lands in M3.
#pragma once
#include "codegen_ir_main.h"

namespace goose {

// ------------------------------------------------------------------
// Load the caller's data stack (gs_stks[gs_sp]) and return it.
// gs_sp is the function's first argument (i64 index into gs_stks).
// gs_stks is a TLS global of type i8** (pointer to an array of
// pointers to gs_stack).
// ------------------------------------------------------------------
inline llvm::Value *ir_load_stack(CodeGenIR &cg, llvm::Function *f) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *gs_sp = f->getArg(0);
    auto *i8ptr = llvm::PointerType::get(*ctx, 0);
    // gs_stks is a TLS i8** global; load it to get the array base.
    auto *stks = b.CreateLoad(i8ptr, cg.gs_stks, "stks");
    // gs_stks[gs_sp] -> the gs_stack pointer.
    // GEP with two indices: [0] is the base (implicit), [gs_sp] is the index.
    std::vector<llvm::Value *> idx = { b.getInt64(0), gs_sp };
    auto *stk_ptr = b.CreateInBoundsGEP(i8ptr, stks, idx, "stk_addr");
    return b.CreateLoad(i8ptr, stk_ptr, "stk");
}

// ------------------------------------------------------------------
// Bump the stack top by n bytes and return the old top (for push).
// If stack_assert is on, also emit an icmp + abort check.
// ------------------------------------------------------------------
inline llvm::Value *ir_bump(CodeGenIR &cg, llvm::Function *f,
                            llvm::Value *n) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *i8ptr = llvm::PointerType::get(*ctx, 0);
    auto *i64 = llvm::Type::getInt64Ty(*ctx);
    auto *stk = ir_load_stack(cg, f);
    // gs_stack layout: { i8* base, i64 top, i64 limit }
    // top is field 1, limit is field 2.
    std::vector<llvm::Type *> stk_fields = { i8ptr, i64, i64 };
    auto *stk_ty = llvm::StructType::get(*ctx, stk_fields);

    // top_ptr = &stk->top (field index 1)
    std::vector<llvm::Value *> top_idx = { b.getInt64(0), b.getInt64(1) };
    auto *top_ptr = b.CreateInBoundsGEP(stk_ty, stk, top_idx, "top_ptr");
    auto *old_top = b.CreateLoad(i64, top_ptr, "old_top");
    auto *new_top = b.CreateAdd(old_top, n, "new_top");

    if (cg.stack_assert) {
        // Check new_top <= limit before writing.
        std::vector<llvm::Value *> lim_idx = { b.getInt64(0), b.getInt64(2) };
        auto *limit_ptr = b.CreateInBoundsGEP(stk_ty, stk, lim_idx, "limit_ptr");
        auto *limit = b.CreateLoad(i64, limit_ptr, "limit");
        auto *over = b.CreateICmpSGT(new_top, limit, "over");
        auto *overrun_bb = llvm::BasicBlock::Create(*ctx, "overrun", f);
        auto *ok_bb = llvm::BasicBlock::Create(*ctx, "bump_ok", f);
        b.CreateCondBr(over, overrun_bb, ok_bb);
        b.SetInsertPoint(overrun_bb);
        std::vector<llvm::Value *> abort_args = { b.getInt64(0) };
        b.CreateCall(cg.f_gs_abort, abort_args);
        b.CreateBr(ok_bb);
        b.SetInsertPoint(ok_bb);
    }

    b.CreateStore(new_top, top_ptr);
    return old_top;
}

// ------------------------------------------------------------------
// Load a value of the given type from the data stack at a byte
// offset from the stack's base.
// ------------------------------------------------------------------
inline llvm::Value *ir_stack_load(CodeGenIR &cg, llvm::Function *f,
                                  llvm::Value *offset, llvm::Type *ty) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *i8ptr = llvm::PointerType::get(*ctx, 0);
    auto *i64 = llvm::Type::getInt64Ty(*ctx);
    auto *stk = ir_load_stack(cg, f);
    std::vector<llvm::Type *> stk_fields = { i8ptr, i64, i64 };
    auto *stk_ty = llvm::StructType::get(*ctx, stk_fields);

    // base_ptr = &stk->base (field index 0)
    std::vector<llvm::Value *> base_idx = { b.getInt64(0), b.getInt64(0) };
    auto *base_ptr = b.CreateInBoundsGEP(stk_ty, stk, base_idx, "base_ptr");
    auto *base = b.CreateLoad(i8ptr, base_ptr, "base");
    // addr = base + offset (byte offset, GEP on i8*)
    std::vector<llvm::Value *> addr_idx = { offset };
    auto *addr = b.CreateInBoundsGEP(i8ptr, base, addr_idx, "addr");
    return b.CreateLoad(ty, addr, "val");
}

// ------------------------------------------------------------------
// Store a value to the data stack at a byte offset from base.
// ------------------------------------------------------------------
inline void ir_stack_store(CodeGenIR &cg, llvm::Function *f,
                           llvm::Value *offset, llvm::Value *val) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *i8ptr = llvm::PointerType::get(*ctx, 0);
    auto *i64 = llvm::Type::getInt64Ty(*ctx);
    auto *stk = ir_load_stack(cg, f);
    std::vector<llvm::Type *> stk_fields = { i8ptr, i64, i64 };
    auto *stk_ty = llvm::StructType::get(*ctx, stk_fields);

    std::vector<llvm::Value *> base_idx = { b.getInt64(0), b.getInt64(0) };
    auto *base_ptr = b.CreateInBoundsGEP(stk_ty, stk, base_idx, "base_ptr");
    auto *base = b.CreateLoad(i8ptr, base_ptr, "base");
    std::vector<llvm::Value *> addr_idx = { offset };
    auto *addr = b.CreateInBoundsGEP(i8ptr, base, addr_idx, "addr");
    b.CreateStore(val, addr);
}

} // namespace goose
