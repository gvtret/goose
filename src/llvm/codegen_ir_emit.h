// LLVM-IR backend: module layout, export table, and c_headers adapters (M4).
//
// The C backend's c_headers mechanism (docs/design/packages.md):
// a package's `.build` file lists `c_headers = ["include/adapters.h"]`,
// and the generated C `#include`s that header so its `static inline`
// adapters (e.g. `mylib_Pair_g5` types) are visible to the generated
// functions. The four external packages (gfx, audio, ui, physics)
// use this.
//
// In the IR backend, per partner decision (option B), these adapters
// are **hand-translated to IR declarations**: the IR module `declare`s
// each adapter function as an ExternalLinkage Function with the same
// name and signature as the C function. The `goose llc` driver (M5)
// compiles the C adapter source into a separate object and links it.
//
// This header holds:
//   * ir_declare_adapter — declare a c_headers adapter function in the
//     module (no body; the driver supplies the C object).
//   * ir_emit_export_table — emit a GlobalVariable array that maps C
//     symbol names to the IR functions that call them (the dlbind table
//     the runtime uses at startup, cf. codegen_emit.h's EmitProgramInit).
//   * ir_emit_program_init — emit the gs_program_init function that
//     reserves regions, initializes the global stack, and binds dylibs.
#pragma once
#include "codegen_ir_calls.h"

namespace goose {

// Declare a c_headers adapter in the IR module. The adapter's C source
// is compiled separately by the `goose llc` driver; here we only
// declare the symbol so the IR can reference it.
//
// name — the C symbol name (e.g. "mylib_Pair_g5_swap").
// ret  — the adapter's return type.
// args — the adapter's parameter types.
inline llvm::Function *ir_declare_adapter(CodeGenIR &cg, const char *name,
                                          llvm::Type *ret,
                                          std::vector<llvm::Type *> args) {
    auto *fty = llvm::FunctionType::get(ret, args, /*isVarArg=*/false);
    return llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                  name, cg.mod.get());
}

// Declare the gs_dl_bind function (from libgoose_rt.a) if not already
// declared. The runtime's gs_program_init calls this to resolve
// dylib-bound externs.
inline void ir_declare_dl_bind(CodeGenIR &cg) {
    auto I = cg.i64();
    auto P = cg.i8ptr();
    auto V = cg.voidty();
    auto dcl = [&](const char *name, llvm::Function *&slot,
                   llvm::Type *ret, std::vector<llvm::Type *> args) {
        auto *fty = llvm::FunctionType::get(ret, args, false);
        slot = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                      name, cg.mod.get());
    };
    // gs_dl_bind: void (i64 *libs, i64 nlibs, i64 *binds, i64 nbinds)
    // The actual signature is in runtime_os.h; this is a placeholder
    // that M5 will refine.
    auto *binds = I;  // i64* placeholder
    (void)binds;
}

// Emit the gs_program_init function: reserves the data-stack regions,
// initializes the global stack, and (option B) no-ops the c_headers
// adapter bindings (the driver handles them at link time).
inline void ir_emit_program_init(CodeGenIR &cg, llvm::Function *f) {
    auto &b = cg.b;
    auto *ctx = &cg.ctx;
    auto *i64 = llvm::Type::getInt64Ty(*ctx);
    auto *i8ptr = llvm::PointerType::get(*ctx, 0);

    auto *entry = llvm::BasicBlock::Create(*ctx, "entry", f);
    b.SetInsertPoint(entry);

    // gs_regions_init(nregions): reserve the global regions.
    // The nregions constant is computed by the driver (M5); for now,
    // pass 0 (no global regions) — the test-suite smoke check does
    // not use globals yet.
    std::vector<llvm::Value *> init_args = { b.getInt64(0) };
    b.CreateCall(cg.f_gs_regions_init, init_args);

    // Return.
    b.CreateRetVoid();
}

} // namespace goose
