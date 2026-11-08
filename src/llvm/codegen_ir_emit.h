// LLVM-IR backend: module layout, export table, c_headers adapters.
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
// This header holds ir_declare_adapter so far. The program entry and
// its data-stack reservation are codegen_ir_main.h's
// emit_program_entry (the earlier gs_regions_init sketch is gone,
// the runtime offers no such symbol). The dlbind export table lands
// with M5.
#pragma once
#include "codegen_ir_main.h"

namespace goose {

// Declare a c_headers adapter in the IR module. The adapter's C source
// is compiled separately by the `goose llc` driver; here we only
// declare the symbol so the IR can reference it.
//
// name: the C symbol name (e.g. "mylib_Pair_g5_swap").
// ret:  the adapter's return type.
// args: the adapter's parameter types.
inline llvm::Function *ir_declare_adapter(CodeGenIR &cg, const char *name,
                                          llvm::Type *ret,
                                          std::vector<llvm::Type *> args) {
    auto *fty = llvm::FunctionType::get(ret, args, /*isVarArg=*/false);
    return llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                  name, cg.mod.get());
}

} // namespace goose
