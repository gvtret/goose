// LLVM-IR backend: the CodeGenIR class.
//
// Mirrors the shape of the C backend's CodeGen (src/codegen.h:90): it takes
// an Ast & and a list of FnSpec specializations and emits the whole program
// into one llvm::Module. The module is the driver's (M5) input to llc.
//
// The class does not re-implement BCE; it sees the BCE-reduced AST.
// TLS state (gs_stks, gs_gl, etc.) lives in the program TU, not in this
// module; the module only declares the gs_* runtime symbols it calls.
//
// See docs/superpowers/specs/2026-11-07-llvm-backend-design.md and
// docs/design/llvm_backend.md.
#pragma once
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/GlobalValue.h>
#include <memory>
#include <string>
#include "includes.h"
#include "ast.h"

namespace goose {

struct CodeGenIR {
    Ast &ast;
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::IRBuilder<> b;

    // The FnSpec specializations to emit (same list CodeGen receives).
    vector<FnSpec *> specs;

    // Whether to insert --stack-assert checks at bump sites.
    bool stack_assert = false;

    // The six TLS globals. All GeneralDynamicTLSModel (partner decision 3).
    llvm::GlobalVariable *gs_gl = nullptr;
    llvm::GlobalVariable *gs_stks = nullptr;
    llvm::GlobalVariable *gs_nstks = nullptr;
    llvm::GlobalVariable *gs_current_thread_id = nullptr;
    llvm::GlobalVariable *gs_regions = nullptr;
    llvm::GlobalVariable *gs_regions_cap = nullptr;

    // Runtime externs (from libgoose_rt.a). Declared, not defined.
    llvm::Function *f_gs_abort = nullptr;
    llvm::Function *f_gs_reserve_region = nullptr;
    llvm::Function *f_gs_regions_init = nullptr;
    llvm::Function *f_gs_thread_spawn = nullptr;
    llvm::Function *f_gs_qput = nullptr;
    llvm::Function *f_gs_qget = nullptr;

    CodeGenIR(Ast &a, vector<FnSpec *> &specs, bool stack_assert)
        : ast(a), ctx(),
          mod(std::make_unique<llvm::Module>("goose_program", ctx)),
          b(ctx) {
        this->specs = specs;
        this->stack_assert = stack_assert;
        emit_tls_globals();
        declare_runtime_externs();
    }

    llvm::Module &module() { return *mod; }

    // Convenience type getters (LLVM 20 API: pointers are PointerType::get,
    // no getInt8PtrTy; no AtomicityManager).
    llvm::Type *i8ptr()  { return llvm::PointerType::get(ctx, 0); }
    llvm::Type *i64()    { return llvm::Type::getInt64Ty(ctx); }
    llvm::Type *voidty() { return llvm::Type::getVoidTy(ctx); }
    llvm::Type *i1()     { return llvm::Type::getInt1Ty(ctx); }

    // ------------------------------------------------------------------
    // TLS globals (GeneralDynamicTLSModel).
    // The runtime's GS_TLS state (gs_stks, gs_gl, etc.) lives in the
    // program TU. The IR module declares them as thread-local globals
    // so the driver can resolve them at link time.
    // ------------------------------------------------------------------
    void emit_tls_globals() {
        auto tls = llvm::GlobalValue::GeneralDynamicTLSModel;

        auto mk = [&](llvm::GlobalVariable *&slot, llvm::Type *ty,
                      const char *name) {
            slot = new llvm::GlobalVariable(*mod, ty, /*isConstant=*/false,
                                            llvm::GlobalValue::CommonLinkage,
                                            nullptr, name,
                                            nullptr, tls);
        };
        mk(gs_gl, i8ptr(), "gs_gl");
        mk(gs_stks, i8ptr(), "gs_stks");
        mk(gs_nstks, i64(), "gs_nstks");
        mk(gs_current_thread_id, i64(), "gs_current_thread_id");
        mk(gs_regions, i8ptr(), "gs_regions");
        mk(gs_regions_cap, i64(), "gs_regions_cap");
    }

    // ------------------------------------------------------------------
    // Runtime externs (from libgoose_rt.a).
    // The set is the GS_API symbols; for now declare the most critical
    // ones that the test-suite smoke check will exercise. The full set
    // is added as M3/M4 needs them.
    // ------------------------------------------------------------------
    void declare_runtime_externs() {
        auto dcl = [&](const char *name, llvm::Function *&slot,
                       llvm::Type *ret, std::vector<llvm::Type *> args) {
            auto *fty = llvm::FunctionType::get(ret, args, /*isVarArg=*/false);
            slot = llvm::Function::Create(fty, llvm::GlobalValue::ExternalLinkage,
                                          name, mod.get());
        };

        auto I = i64();
        auto P = i8ptr();
        auto V = voidty();
        dcl("gs_abort", f_gs_abort, V, { I });
        dcl("gs_reserve_region", f_gs_reserve_region, P, { I, I });
        dcl("gs_regions_init", f_gs_regions_init, V, { I });
        dcl("gs_thread_spawn", f_gs_thread_spawn, I, { P, P, I });
        dcl("gs_qput", f_gs_qput, I, { P, P, I, I });
        dcl("gs_qget", f_gs_qget, I, { P, P, I, I });
    }

    // ------------------------------------------------------------------
    // Emit the program. For now this creates the function shells for
    // each FnSpec; the full body-walk lands in M3.
    // ------------------------------------------------------------------
    void Run() {
        auto i64ty = i64();
        // Every function takes i64 gs_sp as its first argument.
        // The C backend's convention (codegen_frames.h:623) is int64_t gs_sp.
        auto *fnty = llvm::FunctionType::get(i64ty, { i64ty }, false);
        for (auto *sp : specs) {
            // The C name of this specialization (sp->sf->cname or the mangled name).
            auto name = sp->sf->cname.empty() ? sp->sf->name : sp->sf->cname;
            auto *f = llvm::Function::Create(fnty,
                                             llvm::GlobalValue::PrivateLinkage,
                                             name, mod.get());
            auto *entry = llvm::BasicBlock::Create(ctx, "entry", f);
            b.SetInsertPoint(entry);
            // M3: walk sp->body here. For now, return 0.
            b.CreateRet(llvm::ConstantInt::get(i64ty, 0));
        }
    }
};

} // namespace goose
