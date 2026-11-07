// LLVM-IR backend: call emission and extern adapters (M4).
//
// Mirrors the C backend's codegen_calls.h / codegen_builtins.h call
// mechanics in LLVM IRBuilder terms:
//
//   * ir_call_fn — emit a direct call to a Goose function (same-module
//     PrivateLinkage). The gs_sp argument is always first.
//   * ir_call_extern — emit a call to an extern fn declared via
//     c_headers adapters or libgoose_rt.a symbols.
//   * ir_thread_spawn — wrapper around gs_thread_spawn for `thread_fn`.
//   * ir_qput / ir_qget — wrappers around gs_qput / gs_qget.
//
// The full Loc / Dst / NrvoDest plumbing (how arguments are placed on
// the data stack and how nonfixed returns are received) lands in M5;
// this header provides the low-level call primitives that M5 builds on.
#pragma once
#include "codegen_ir_values.h"

namespace goose {

// Emit a direct call to a Goose function within the same module.
// args includes gs_sp as the first element when needssp is true.
inline llvm::Value *ir_call_fn(CodeGenIR &cg, llvm::Function *callee,
                               std::vector<llvm::Value *> args) {
    return cg.b.CreateCall(callee, args);
}

// Emit a call to an extern function (libgoose_rt.a or c_headers adapter).
// The callee must already be declared in the module (declare_runtime_externs
// or a c_headers adapter declaration).
inline llvm::Value *ir_call_extern(CodeGenIR &cg, llvm::Function *callee,
                                   std::vector<llvm::Value *> args) {
    return cg.b.CreateCall(callee, args);
}

// Wrapper: gs_thread_spawn(fn_ptr, arg_ptr, arg_size) -> i64 thread_id.
inline llvm::Value *ir_thread_spawn(CodeGenIR &cg, llvm::Value *fn_ptr,
                                    llvm::Value *arg_ptr, llvm::Value *arg_size) {
    std::vector<llvm::Value *> args = { fn_ptr, arg_ptr, arg_size };
    return ir_call_extern(cg, cg.f_gs_thread_spawn, args);
}

// Wrapper: gs_qput(q, val_ptr, val_size, timeout) -> i64 status.
inline llvm::Value *ir_qput(CodeGenIR &cg, llvm::Value *q,
                            llvm::Value *val_ptr, llvm::Value *val_size,
                            llvm::Value *timeout) {
    std::vector<llvm::Value *> args = { q, val_ptr, val_size, timeout };
    return ir_call_extern(cg, cg.f_gs_qput, args);
}

// Wrapper: gs_qget(q, dst_ptr, dst_size, timeout) -> i64 status.
inline llvm::Value *ir_qget(CodeGenIR &cg, llvm::Value *q,
                            llvm::Value *dst_ptr, llvm::Value *dst_size,
                            llvm::Value *timeout) {
    std::vector<llvm::Value *> args = { q, dst_ptr, dst_size, timeout };
    return ir_call_extern(cg, cg.f_gs_qget, args);
}

} // namespace goose