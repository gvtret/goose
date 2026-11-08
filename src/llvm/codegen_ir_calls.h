// LLVM-IR backend: extern call plumbing.
//
// Only the model-independent wrapper so far; the thread/queue wrappers
// (`gs_thread_start`, `gs_qput`, `gs_qget`) come with the M4 lowering
// that defines their real signatures (runtime.h:969, 996-997), not the
// fictitious `gs_thread_spawn` shape sketched here earlier.
#pragma once
#include "codegen_ir_main.h"

namespace goose {

// Call a declared extern with the given arguments.
inline llvm::Value *ir_call_extern(CodeGenIR &cg, llvm::Function *callee,
                                   std::vector<llvm::Value *> args) {
    return cg.b.CreateCall(callee, args);
}

} // namespace goose
