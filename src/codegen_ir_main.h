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
                                            llvm::GlobalValue::ExternalLinkage,
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

    // Runtime extern: gs_print_i64(i64 val) — prints a signed 64-bit integer
    // followed by a newline. Declared here for the minimal body-walk; the
    // symbol lives in libgoose_rt.a (or a stub the driver links).
    llvm::Function *f_gs_print_i64 = nullptr;

    void declare_print_extern() {
        auto *fty = llvm::FunctionType::get(voidty(), { i64() }, false);
        f_gs_print_i64 = llvm::Function::Create(fty,
                                                 llvm::GlobalValue::ExternalLinkage,
                                                 "gs_print_i64", mod.get());
    }

    // ------------------------------------------------------------------
    // Minimal M3 body-walk: IntLit, Binary (+,-,*,/), Call(print), Block.
    // Returns the i64 value of an expression, or nullptr for statements.
    // ------------------------------------------------------------------
    llvm::Value *GenExpr(Node *n, llvm::Function *f) {
        if (!n) return nullptr;
        if (auto *il = Is<IntLit>(n))
            return llvm::ConstantInt::get(i64(), (uint64_t)il->val, true);
        if (auto *bin = Is<Binary>(n)) {
            auto *l = GenExpr(bin->left, f);
            auto *r = GenExpr(bin->right, f);
            if (!l || !r) return nullptr;
            switch (bin->op) {
                case T_PLUS:  return b.CreateAdd(l, r, "add");
                case T_MINUS: return b.CreateSub(l, r, "sub");
                case T_MUL:   return b.CreateMul(l, r, "mul");
                case T_DIV:   return b.CreateSDiv(l, r, "div");
                default:      return nullptr;
            }
        }
        if (auto *c = Is<Call>(n)) {
            // Only print(i64) is supported in this minimal walk.
            if (c->args.size() == 1 && f_gs_print_i64) {
                auto *arg = GenExpr(c->args[0], f);
                if (arg) b.CreateCall(f_gs_print_i64, { arg });
            }
            return nullptr;
        }
        return nullptr;
    }

    void GenStmt(Node *n, llvm::Function *f) {
        if (!n) return;
        if (auto *blk = Is<Block>(n)) {
            for (auto *s : blk->stmts) GenStmt(s, f);
            if (blk->tail) GenExpr(blk->tail, f);
            return;
        }
        // Expression-as-statement (e.g. print(...)).
        GenExpr(n, f);
    }

    // ------------------------------------------------------------------
    // Emit the program. Creates function shells and walks bodies for
    // the minimal M3 subset.
    // ------------------------------------------------------------------
    void Run() {
        declare_print_extern();
        auto i64ty = i64();
        auto *fnty = llvm::FunctionType::get(i64ty, { i64ty }, false);
        for (auto *sp : specs) {
            auto name = sp->sf->cname.empty() ? sp->sf->name : sp->sf->cname;
            auto *fn = llvm::Function::Create(fnty,
                                              llvm::GlobalValue::ExternalLinkage,
                                              name, mod.get());
            auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            b.SetInsertPoint(entry);
            if (sp->body) GenStmt(sp->body, fn);
            b.CreateRet(llvm::ConstantInt::get(i64ty, 0));
        }
    }
};

} // namespace goose
