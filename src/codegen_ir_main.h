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
#include <llvm/IR/Instructions.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "includes.h"
#include "ast.h"
#include "builtins.h"

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
        declare_entry_externs();
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

    // ------------------------------------------------------------------
    // Body-walk state: local i64 storage (alloca) and emitted function
    // shells, per function body.
    // ------------------------------------------------------------------
    struct FnWalk {
        llvm::Function *fn = nullptr;
        llvm::BasicBlock *entry = nullptr;
        // VarDef * (from the walked spec) -> alloca for its i64 local.
        std::unordered_map<VarDef *, llvm::AllocaInst *> locals;
        // Parameter slots: VarDef * -> llvm::Argument * (i64 params).
        std::unordered_map<VarDef *, llvm::Value *> params;
        // The SFunction the walk body belongs to (the spec's sf). A Return
        // whose target differs from this exits an enclosing InlineBlock, not
        // the function: the value flows out as that block's result.
        SFunction *cursf = nullptr;
        // The walked body Block (only the function's own; nested blocks are
        // not top-level, so their value tails do not become rets).
        Block *body = nullptr;
        // A value a statement handed up out of its own block (an inlined
        // callee's return, a break carrying the block's value); the enclosing
        // block or InlineBlock consumes it.
        llvm::Value *flowout = nullptr;
    };

    // Number of live specializations per qualified Goose function name. The
    // C backend suffixes every overloaded or multiply-specialized name.
    std::unordered_map<string_view, int> name_counts;

    // The i64 types a walk accepts: i8..i64/u8..u64 all as i64, for now.
    static bool is_int_ty(TypeExpr *t) {
        return t && t->kind == TY_INT && t->intstorage != IS_VARINT;
    }

    // The runtime's print(i64) entry. `print` is B_PRINT in builtins.h.
    // The minimal walk calls gs_print_i64 (a libgoose_rt.a stub) instead of
    // the full gs_out_bytes rendering path, which M3 replaces with EmitFormatInto.
    llvm::Function *f_gs_print_i64 = nullptr;
    void declare_print_extern() {
        auto *fty = llvm::FunctionType::get(voidty(), { i64() }, false);
        f_gs_print_i64 = llvm::Function::Create(fty,
                                                 llvm::GlobalValue::ExternalLinkage,
                                                 "gs_print_i64", mod.get());
    }

    // ------------------------------------------------------------------
    // Minimal M3 body-walk: IntLit, Binary (+,-,*,/,%), Call (user fns,
    // print), Block, VarDecl (i64 let/var), Assign (=, +=, -=, *=, /=),
    // IncDec (++/--), Return (i64).
    // Values are i64; all other types are not yet walked.
    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    // Emit the program. Creates function shells and walks bodies for
    // the minimal M3 subset.
    // ------------------------------------------------------------------
    void Run() {
        declare_print_extern();
        auto i64ty = i64();

        // Pass 1: create every function's shell so a later body walk can
        // resolve a call to an earlier-emitted function (getFunction).
        name_counts.clear();
        for (auto *sp : specs) ++name_counts[sp->sf->qname];
        struct Shell {
            FnSpec *sp;
            llvm::Function *fn;
            bool supported;
            string suffix;
        };
        // The Goose entry function is named `goose_main` in the module (not
        // `main`): the C entry point `int main()` is emitted separately below,
        // and a `main` here would collide with it at link time.
        // A spec's emitted name carries its spec ID when the function has
        // several live specializations, keeping the symbols distinct.
        vector<Shell> shells;
        for (auto *sp : specs) {
            string name = sp->sf->cname.empty() ? string(sp->sf->name) : sp->sf->cname;
            auto suffix = name_counts[sp->sf->qname] > 1
                ? cat("_", sp->id) : string();
            auto mainsf = ast.MainFunction();
            if (mainsf && sp->sf == mainsf) name = "goose_main";
            std::vector<llvm::Type *> argtys;
            bool supported = true;
            for (auto *t : sp->argtypes) {
                if (is_int_ty(t)) argtys.push_back(i64ty);
                else supported = false;
            }
            llvm::Type *retty = voidty();
            if (!sp->rets.empty()) {
                retty = is_int_ty(sp->rets[0]) ? i64ty : nullptr;
                if (!retty) supported = false;
            }
            auto *fnty = llvm::FunctionType::get(supported ? retty : voidty(),
                                                 supported ? argtys : std::vector<llvm::Type *>(),
                                                 false);
            auto *fn = llvm::Function::Create(fnty,
                                              llvm::GlobalValue::ExternalLinkage,
                                              name + suffix, mod.get());
            shells.push_back({ sp, fn, supported, suffix });
        }

        // Pass 2: walk each body (now every shell exists to be called).
        for (auto &sh : shells) {
            auto *sp = sh.sp;
            auto *fn = sh.fn;
            FnWalk w;
            w.fn = fn;
            w.cursf = sp->sf;
            w.body = sp->body;
            w.entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            b.SetInsertPoint(w.entry);
            // Bind parameters (aligned with argtypes; i64 slots only).
            for (size_t i = 0; i < sp->params.size() && i < fn->getFunctionType()->getNumParams(); ++i)
                w.params[sp->params[i]] = fn->getArg(i);
            if (sp->body && sh.supported) GenStmt(sp->body, w);
            // Close any basic block the walk left unterminated. An if/else
            // merge block, for instance, only terminates when the statement
            // after the if (usually the body tail's implicit return) lands in
            // it; a walk that cannot emit that tail leaves the block open.
            // Every non-terminated block returns the function's default value
            // so the IR always verifies.
            for (auto &bri : *fn) {
                if (!bri.getTerminator()) {
                    b.SetInsertPoint(&bri);
                    if (fn->getReturnType()->isVoidTy()) b.CreateRetVoid();
                    else b.CreateRet(llvm::ConstantInt::get(i64ty, 0));
                }
            }
        }

        emit_program_entry();
    }

    // ------------------------------------------------------------------
    // Body-walk definitions (minimal M3 subset, i64 values).
    // ------------------------------------------------------------------
    llvm::Value *GenExpr(Node *n, FnWalk &w) {
        if (!n) return nullptr;
        if (auto *il = Is<IntLit>(n))
            return llvm::ConstantInt::get(i64(), (uint64_t)il->val, true);
        if (auto *fl = Is<FltLit>(n))
            return llvm::ConstantInt::get(i64(),
                                          (uint64_t)(int64_t)(double)fl->val, true);
        if (auto *bl = Is<BoolLit>(n))
            return llvm::ConstantInt::get(i64(), bl->val ? 1 : 0, true);
        if (auto *un = Is<Unary>(n)) {
            // The unary minus: an i64 negation. The logical not: an i1 test
            // widened back to i64 (the minimal walk's uniform value type).
            // A bit-and (&x) address is not yet walked.
            auto *c = GenExpr(un->child, w);
            if (!c) return nullptr;
            switch (un->op) {
                case T_MINUS: return b.CreateNeg(c, "neg");
                case T_NOT: {
                    // `c` may be i1 (from a comparison) or i64 (from an
                    // integer expression).  Normalise to i1, invert, then
                    // widen back to i64 (the walk's uniform integer type).
                    llvm::Value *cond;
                    if (c->getType()->isIntegerTy(1)) {
                        cond = c;
                    } else {
                        auto *z = llvm::ConstantInt::get(i64(), 0);
                        cond = b.CreateICmpNE(c, z, "tobool");
                    }
                    auto *inv = b.CreateNot(cond, "not");
                    return b.CreateZExt(inv, i64(), "notw");
                }
                default:      return nullptr;
            }
        }
        if (auto *bin = Is<Binary>(n)) {
            auto *l = GenExpr(bin->left, w);
            auto *r = GenExpr(bin->right, w);
            if (!l || !r) return nullptr;
            switch (bin->op) {
                case T_PLUS:  return b.CreateAdd(l, r, "add");
                case T_MINUS: return b.CreateSub(l, r, "sub");
                case T_MUL:   return b.CreateMul(l, r, "mul");
                case T_DIV:   return b.CreateSDiv(l, r, "div");
                case T_MOD:   return b.CreateSRem(l, r, "mod");
                case T_EQ:    return b.CreateICmpEQ(l, r, "eq");
                case T_NEQ:   return b.CreateICmpNE(l, r, "ne");
                case T_LT:    return b.CreateICmpSLT(l, r, "lt");
                case T_GT:    return b.CreateICmpSGT(l, r, "gt");
                case T_LTEQ:  return b.CreateICmpSLE(l, r, "le");
                case T_GTEQ:  return b.CreateICmpSGE(l, r, "ge");
                default:      return nullptr;
            }
        }
        if (auto *id = Is<Ident>(n)) {
            if (id->vdef) {
                auto pit = w.params.find(id->vdef);
                if (pit != w.params.end()) return pit->second;
                auto lit = w.locals.find(id->vdef);
                if (lit != w.locals.end())
                    return b.CreateLoad(i64(), lit->second, llvm::Twine(id->vdef->name));
            }
            return nullptr;
        }
        if (auto *c = Is<Call>(n)) {
            // print(i64) via the libgoose_rt.a stub.
            if (c->builtin == B_PRINT && c->args.size() == 1) {
                auto *arg = GenExpr(c->args[0], w);
                if (arg) b.CreateCall(f_gs_print_i64, { arg });
                return nullptr;
            }
            // A direct call to a Goose function: all i64 args, i64 ret.
            if (c->spec && c->spec->rets.size() == 1 && is_int_ty(c->spec->rets[0])) {
                string callee = c->spec->sf->cname.empty()
                    ? string(c->spec->sf->name) : c->spec->sf->cname;
                if (name_counts[c->spec->sf->qname] > 1)
                    callee += cat("_", c->spec->id);
                auto *cf = mod->getFunction(callee);
                if (!cf) return nullptr;
                std::vector<llvm::Value *> args;
                for (size_t i = 0; i < c->args.size() && i < cf->getFunctionType()->getNumParams(); ++i) {
                    auto *av = GenExpr(c->args[i], w);
                    if (!av) return nullptr;
                    args.push_back(av);
                }
                return b.CreateCall(cf, args);
            }
            return nullptr;
        }
        if (auto *ib = Is<InlineBlock>(n)) {
            // An inlined call: walk its body's statements (the parameter
            // bindings and the callee's body), then its tail value.
            if (ib->body) {
                for (auto *s : ib->body->stmts) GenStmt(s, w);
                if (ib->body->tail) {
                    auto *tv = GenExpr(ib->body->tail, w);
                    if (tv) return tv;
                }
            }
            return nullptr;
        }
        if (auto *eb = Is<EarlyBlock>(n)) {
            // A `block { ... }` expression: walk its body's statements and
            // return the body's tail value (a value-carrying break).
            if (eb->body) {
                for (auto *s : eb->body->stmts) GenStmt(s, w);
                if (eb->body->tail) {
                    auto *tv = GenExpr(eb->body->tail, w);
                    if (tv) return tv;
                }
            }
            return nullptr;
        }
        if (auto *ifx = Is<IfExpr>(n)) {
            // if cond { then } [else { e }] as an expression or statement.
            // The branches run in their own blocks; a branch that leaves the
            // function (a return) terminates it and contributes no value.
            // When both branches yield a value, they meet in a PHI at the
            // merge (the if-expression's value).
            auto *cond = GenExpr(ifx->cond, w);
            if (!cond) return nullptr;
            if (!cond->getType()->isIntegerTy(1)) {
                auto *z = llvm::ConstantInt::get(i64(), 0);
                cond = b.CreateICmpNE(cond, z, "ifcond");
            }
            auto *fn = w.fn;
            auto *thenbb = llvm::BasicBlock::Create(ctx, "then", fn);
            auto *elsebb = ifx->elseb ? llvm::BasicBlock::Create(ctx, "else", fn) : nullptr;
            auto *merge = llvm::BasicBlock::Create(ctx, "ifmerge", fn);
            b.CreateCondBr(cond, thenbb, elsebb ? elsebb : merge);
            // A branch's trailing value once its statements have run (a branch
            // that returned has no value and its block is already terminated).
            auto branchval = [&](Block *bb, llvm::BasicBlock *where) -> llvm::Value * {
                if (!bb || !bb->tail) return nullptr;
                if (where->getTerminator()) return nullptr;   // Branch returned.
                b.SetInsertPoint(where);
                return GenExpr(bb->tail, w);
            };
            b.SetInsertPoint(thenbb);
            if (ifx->thenb) GenStmt(ifx->thenb, w);
            llvm::Value *thenval = branchval(ifx->thenb, b.GetInsertBlock());
            if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(merge);
            llvm::Value *elseval = nullptr;
            if (elsebb) {
                b.SetInsertPoint(elsebb);
                if (auto *eb = Is<Block>(ifx->elseb)) GenStmt(eb, w);
                else if (ifx->elseb)
                    elseval = GenExpr(ifx->elseb, w);
                if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(merge);
                if (!elseval) {
                    auto *eb = Is<Block>(ifx->elseb);
                    if (eb) elseval = branchval(eb, b.GetInsertBlock());
                }
            }
            b.SetInsertPoint(merge);
            if (thenval && elseval && thenval != elseval) {
                auto *phi = b.CreatePHI(i64(), 2, "ifval");
                phi->addIncoming(thenval, thenbb);
                phi->addIncoming(elseval, elsebb);
                return phi;
            }
            return thenval ? thenval : elseval;
        }
        return nullptr;
    }

    void GenStmt(Node *n, FnWalk &w) {
        if (!n) return;
        if (auto *blk = Is<Block>(n)) {
            // Only the walked function's own body ends in an implicit return;
            // a nested block's tail value flows back to the enclosing
            // expression, not a ret.
            auto top = blk == w.body;
            for (auto *s : blk->stmts) GenStmt(s, w);
            // A nested block's flow-out value (an inlined callee's return)
            // is this block's value.
            if (w.flowout) {
                llvm::Value *v = w.flowout;
                w.flowout = nullptr;
                if (top && v && w.fn->getReturnType()->isIntegerTy(64) &&
                    !b.GetInsertBlock()->getTerminator())
                    b.CreateRet(v);
                return;
            }
            if (blk->tail) {
                // A tail that is an explicit return statement.
                if (auto *tr = Is<Return>(blk->tail)) {
                    GenStmt(tr, w);
                    return;
                }
                // A value-producing tail of the top-level body: the implicit
                // return. Nested blocks evaluate their tail for effect and
                // value only.
                auto *tv = GenExpr(blk->tail, w);
                if (top && tv && w.fn->getReturnType()->isIntegerTy(64) &&
                    !b.GetInsertBlock()->getTerminator())
                    b.CreateRet(tv);
            }
            return;
        }
        if (auto *ib = Is<InlineBlock>(n)) {
            // An inlined call body: walk its inner block, then its tail.
            // A `return` whose target is the inlined callee (not the walked
            // function) lands in w.flowout; the value then flows out of this
            // statement as the call's result.
            auto *prevout = w.flowout;
            w.flowout = nullptr;
            if (ib->body) {
                for (auto *s : ib->body->stmts) GenStmt(s, w);
                if (w.flowout) {
                    w.flowout = prevout;
                    return;
                }
                if (ib->body->tail) {
                    auto *tv = GenExpr(ib->body->tail, w);
                    if (tv && w.fn->getReturnType()->isIntegerTy(64) &&
                        !b.GetInsertBlock()->getTerminator())
                        b.CreateRet(tv);
                }
            }
            w.flowout = prevout;
            return;
        }
        if (auto *vd = Is<VarDecl>(n)) {
            // An i64 let/var with an initializer: an alloca + store.
            for (size_t i = 0; i < vd->defs.size(); ++i) {
                auto *vddef = vd->defs[i];
                if (!vddef || !is_int_ty(vddef->type)) continue;
                auto *alloca = b.CreateAlloca(i64(), nullptr, llvm::Twine(vddef->name));
                if (i < vd->inits.size() && vd->inits[i]) {
                    auto *initv = GenExpr(vd->inits[i], w);
                    if (initv) b.CreateStore(initv, alloca);
                }
                w.locals[vddef] = alloca;
            }
            return;
        }
        if (auto *as = Is<Assign>(n)) {
            // i64 assignment to a local.
            auto *lv = Is<Ident>(as->lval);
            if (!lv || !lv->vdef) return;
            auto *rhs = GenExpr(as->rhs, w);
            if (!rhs) return;
            llvm::Value *cur = nullptr;
            if (as->op != T_ASSIGN) {
                auto pit = w.params.find(lv->vdef);
                if (pit != w.params.end()) {
                    // A parameter is not an lvalue here: rebind into a local.
                    auto *alloca = b.CreateAlloca(i64(), nullptr, llvm::Twine(lv->vdef->name));
                    b.CreateStore(pit->second, alloca);
                    w.locals[lv->vdef] = alloca;
                    cur = pit->second;
                } else {
                    auto lit = w.locals.find(lv->vdef);
                    if (lit == w.locals.end()) return;
                    cur = b.CreateLoad(i64(), lit->second, llvm::Twine(lv->vdef->name));
                }
                switch (as->op) {
                    case T_PLUSEQ:  rhs = b.CreateAdd(cur, rhs, "pluseq"); break;
                    case T_MINUSEQ: rhs = b.CreateSub(cur, rhs, "minuseq"); break;
                    case T_MULEQ:   rhs = b.CreateMul(cur, rhs, "muleq"); break;
                    case T_DIVEQ:   rhs = b.CreateSDiv(cur, rhs, "diveq"); break;
                    default: return;
                }
            }
            auto pit2 = w.params.find(lv->vdef);
            auto lit2 = w.locals.find(lv->vdef);
            if (pit2 != w.params.end()) {
                // Plain assign to a parameter: store into a shadow alloca.
                if (lit2 == w.locals.end()) {
                    auto *alloca = b.CreateAlloca(i64(), nullptr, llvm::Twine(lv->vdef->name));
                    b.CreateStore(pit2->second, alloca);
                    w.locals[lv->vdef] = alloca;
                    lit2 = w.locals.find(lv->vdef);
                }
                b.CreateStore(rhs, lit2->second);
            } else if (lit2 != w.locals.end()) {
                b.CreateStore(rhs, lit2->second);
            }
            return;
        }
        if (auto *wh = Is<While>(n)) {
            auto *fn = w.fn;
            auto *condbb = llvm::BasicBlock::Create(ctx, "while_cond", fn);
            auto *bodybb = llvm::BasicBlock::Create(ctx, "while_body", fn);
            auto *donebb = llvm::BasicBlock::Create(ctx, "while_done", fn);
            b.CreateBr(condbb);
            b.SetInsertPoint(condbb);
            auto *cond = GenExpr(wh->cond, w);
            if (!cond) return;
            if (!cond->getType()->isIntegerTy(1)) {
                auto *z = llvm::ConstantInt::get(i64(), 0);
                cond = b.CreateICmpNE(cond, z, "while_test");
            }
            b.CreateCondBr(cond, bodybb, donebb);
            b.SetInsertPoint(bodybb);
            if (wh->body) GenStmt(wh->body, w);
            if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(condbb);
            b.SetInsertPoint(donebb);
            return;
        }
        if (auto *fl = Is<ForLoop>(n)) {
            // First loop subset: `for i in count` and `for i in lo..hi`.
            // The counter is i64 in the minimal walk; array/slice iteration,
            // narrow counter types, and index binders remain M3 work.
            if (fl->iterkind == IK_COUNT || fl->iterkind == IK_RANGE) {
                llvm::Value *lo = llvm::ConstantInt::get(i64(), 0);
                llvm::Value *hi = GenExpr(fl->iter, w);
                if (auto *r = Is<RangeExpr>(fl->iter)) {
                    lo = GenExpr(r->lo, w);
                    hi = GenExpr(r->hi, w);
                }
                if (!lo || !hi) return;
                auto *fn = w.fn;
                auto *condbb = llvm::BasicBlock::Create(ctx, "for_cond", fn);
                auto *bodybb = llvm::BasicBlock::Create(ctx, "for_body", fn);
                auto *donebb = llvm::BasicBlock::Create(ctx, "for_done", fn);
                b.CreateBr(condbb);
                b.SetInsertPoint(condbb);
                auto *iv = b.CreatePHI(i64(), 2, "for_i");
                iv->addIncoming(lo, b.GetInsertBlock()->getSinglePredecessor());
                auto *cond = b.CreateICmpSLT(iv, hi, "for_test");
                b.CreateCondBr(cond, bodybb, donebb);
                b.SetInsertPoint(bodybb);
                if (fl->vdef) {
                    auto *slot = b.CreateAlloca(i64(), nullptr, llvm::Twine(fl->vdef->name));
                    b.CreateStore(iv, slot);
                    w.locals[fl->vdef] = slot;
                }
                if (fl->body) GenStmt(fl->body, w);
                auto *body_end = b.GetInsertBlock();
                if (!body_end->getTerminator()) {
                    b.SetInsertPoint(body_end);
                    auto *next = b.CreateAdd(iv, llvm::ConstantInt::get(i64(), 1), "for_next");
                    iv->addIncoming(next, body_end);
                    b.CreateBr(condbb);
                }
                b.SetInsertPoint(donebb);
                return;
            }
            return;
        }
        if (auto *brk = Is<Break>(n)) {
            // A `break` out of an EarlyBlock: its value (or the block's
            // fall-through) is the block's result, handed up as the flow-out.
            w.flowout = brk->val ? GenExpr(brk->val, w) : nullptr;
            return;
        }
        if (auto *inc = Is<IncDec>(n)) {
            auto *lv = Is<Ident>(inc->lval);
            if (!lv || !lv->vdef) return;
            auto lit = w.locals.find(lv->vdef);
            if (lit == w.locals.end()) {
                auto pit = w.params.find(lv->vdef);
                if (pit == w.params.end()) return;
                auto *alloca = b.CreateAlloca(i64(), nullptr, llvm::Twine(lv->vdef->name));
                b.CreateStore(pit->second, alloca);
                w.locals[lv->vdef] = alloca;
                lit = w.locals.find(lv->vdef);
            }
            auto *one = llvm::ConstantInt::get(i64(), 1);
            auto *cur = b.CreateLoad(i64(), lit->second, llvm::Twine(lv->vdef->name));
            auto *nv = inc->op == T_INC ? b.CreateAdd(cur, one, "inc")
                                        : b.CreateSub(cur, one, "dec");
            b.CreateStore(nv, lit->second);
            return;
        }
        if (auto *ret = Is<Return>(n)) {
            // A `return ... from f` exits an enclosing scope, not this
            // function: the minimal walk does not model it (the full M3
            // Loc/Dst machinery does), so it is dropped. An inlined callee's
            // own `return` (target not the walked function, no from) is a
            // normal function return of this walk, so it emits as usual.
            if (!ret->from.empty()) return;
            if (ret->target && w.cursf && ret->target != w.cursf) {
                // An inlined callee's return: its value is the call's result,
                // handed to the enclosing InlineBlock (its GenStmt catches
                // w.flowout and, at function top level, emits the ret).
                w.flowout = ret->vals.empty() ? nullptr : GenExpr(ret->vals[0], w);
                return;
            }
            if (ret->vals.empty() && w.fn->getReturnType()->isVoidTy()) {
                b.CreateRetVoid();
                return;
            }
            if (!ret->vals.empty()) {
                auto *v = GenExpr(ret->vals[0], w);
                if (v) b.CreateRet(v);
                return;
            }
            // A value-returning function's bare `return;` defaults to zero
            // (the full walk uses the type's default).
            if (w.fn->getReturnType()->isIntegerTy(64))
                b.CreateRet(llvm::ConstantInt::get(i64(), 0));
            return;
        }
        // Expression-as-statement (e.g. print(...)).
        GenExpr(n, w);
    }

    // ------------------------------------------------------------------
    // Program entry: `int main()` that starts the runtime and calls the
    // Goos main(). Mirrors the C backend's EmitMain (codegen_emit.h:1119).
    // ------------------------------------------------------------------
    void emit_program_entry() {
        auto mainsf = ast.MainFunction();
        if (!mainsf) return;
        auto *mainspec = mainsf->specs.empty() ? nullptr : mainsf->specs[0];
        if (!mainspec) return;
        auto i64ty = i64();
        auto *i32ty = llvm::Type::getInt32Ty(ctx);

        // C entry: int main() -> the Goose main (renamed `goose_main` to
        // avoid the link collision with the C runtime's own symbol). The
        // full runtime init (gs_rt_init, gs_gl) stays in the driver's C
        // object for now; it lands with the data-stack walk (M3 completion).
        auto *mfn = mod->getFunction("goose_main");
        auto *mfnty = llvm::FunctionType::get(i32ty, {}, false);
        auto *mfn2 = llvm::Function::Create(mfnty, llvm::GlobalValue::ExternalLinkage,
                                            "main", mod.get());
        auto *mentry = llvm::BasicBlock::Create(ctx, "entry", mfn2);
        b.SetInsertPoint(mentry);
        if (mfn) b.CreateCall(mfn, {});
        b.CreateRet(llvm::ConstantInt::get(i32ty, 0));
    }

    llvm::Function *f_gs_rt_init = nullptr;
    llvm::Function *f_gs_globals_init = nullptr;

    // Runtime entry externs for the program main.
    void declare_entry_externs() {
        auto *i64ty = i64();
        auto *ptrty = i8ptr();
        auto *fty = llvm::FunctionType::get(voidty(),
            { i64ty, ptrty, i64ty, i64ty, i64ty }, false);
        f_gs_rt_init = llvm::Function::Create(fty,
            llvm::GlobalValue::ExternalLinkage, "gs_rt_init", mod.get());
        auto *gfty = llvm::FunctionType::get(voidty(), {}, false);
        f_gs_globals_init = llvm::Function::Create(gfty,
            llvm::GlobalValue::ExternalLinkage, "gs_globals_init", mod.get());
    }
};

} // namespace goose
