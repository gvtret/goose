// LLVM-IR backend: in-place construction helpers (M3).
//
// The C backend's OpenRzDest / EmitReprefix / EmitSlidePrefix
// (codegen_builtins.h:1391, codegen_calls.h:409) reserve a prefix on
// the data stack for a resizable value's length header, then patch
// the count after the elements are built. This header holds the IR
// equivalents:
//
//   * ir_open_rz_dest — reserve `n` bytes on the stack, return the
//     address where the count will be patched.
//   * ir_patch_prefix — load the current stack top, compute the
//     count, store it to the reserved prefix address.
//
// The full Loc / Dst / NrvoDest plumbing (how a construct's
// destination is chosen and passed to a call) lands in M4.
#pragma once
#include "codegen_ir_values.h"

namespace goose {

// Reserve `n` bytes on the current data stack and return the address
// of the first reserved byte (the prefix, where the count will be
// patched). Mirrors C's OpenRzDest (codegen_builtins.h:1391):
//   `void *pref = Top(stk); Bump(stk, PrefixBytes(ls));`
inline llvm::Value *ir_open_rz_dest(CodeGenIR &cg, llvm::Function *f,
                                    llvm::Value *n) {
    return ir_bump(cg, f, n);
}

// Patch a reserved prefix with the element count. Mirrors C's
// EmitPrefixPatch (codegen_stmts.h:1023):
//   `*pref = (count_ty)count;`
// For varint prefixes (ls == IS_VARINT) the patch writes a ULEB128
// instead; that is a follow-up.
inline void ir_patch_prefix(CodeGenIR &cg, llvm::Function *f,
                            llvm::Value *pref, llvm::Value *count) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *i64 = llvm::Type::getInt64Ty(*ctx);
    // pref is an i8* to the reserved prefix slot; load the byte
    // pointer and store the count as i64.
    b.CreateStore(count, pref);
}

} // namespace goose
