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

    // Runtime externs (from libgoose_rt.a). Declared, not defined. Only
    // symbols the archive really exports (GS_API in runtime.h); the
    // fictitious gs_regions_init/gs_rt_init/thread-queue stubs are gone
    // (Task 3 foundation findings, M2-M6 plan).
    llvm::Function *f_gs_rt_start = nullptr;         // (i32,ptr,i64,i64,i64,i64,i64)
    llvm::Function *f_gs_reserve_region = nullptr;   // (ptr) -> ptr
    llvm::Function *f_gs_idxfail = nullptr;          // (i64,i64,ptr,i32), noreturn
    llvm::Function *f_gs_asfail_i = nullptr;         // (ptr,i64,ptr,ptr,i32), noreturn

    // A growable i64 array local (T[>..]): one dedicated data-stack slot
    // (a lazy thread-local gs_stack { top }) plus the gs_rhdr { base, len }
    // allocas the body walk updates. The slot's region is reserved once in
    // int main() through gs_reserve_region (emit_program_entry); runtime.h
    // 900-930 is the contract.
    std::vector<llvm::GlobalVariable *> stk_slots;

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
    // no getInt8PtrTy; no AtomicityManager). i8ptr() is a PointerType so
    // ConstantPointerNull::get takes it directly.
    llvm::PointerType *i8ptr() { return llvm::PointerType::get(ctx, 0); }
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

        // Exact signatures (runtime.h:922-929): gs_rt_start(argc, argv,
        // reserve, gap, budget, mainregions, workerregions) starts the
        // runtime on the main thread; gs_reserve_region(gs_stack *)
        // reserves one region and returns its base without touching
        // stack->top (the caller stores it); gs_idxfail(i, n, file, line)
        // is the bounds-check abort and does not return.
        auto I = i64();
        auto P = i8ptr();
        auto V = voidty();
        auto I32 = llvm::Type::getInt32Ty(ctx);
        dcl("gs_rt_start", f_gs_rt_start, V, { I32, P, I, I, I, I, I });
        dcl("gs_reserve_region", f_gs_reserve_region, P, { P });
        dcl("gs_idxfail", f_gs_idxfail, V, { I, I, P, I32 });
        dcl("gs_asfail_i", f_gs_asfail_i, V, { P, I, P, P, I32 });
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
        // The enclosing loops of the walk (innermost last); a break or
        // continue without a value targets the innermost one.
        struct Loop {
            llvm::BasicBlock *condbb = nullptr;   // The loop's condition block.
            llvm::BasicBlock *donebb = nullptr;    // The loop's exit block.
        };
        // Growable array locals of this walk: VarDef * -> header. base and
        // len are local allocas (their alloca'd header); the same record
        // shape also carries a global's thread-local header (storage
        // shared across bodies instead of a stack frame's).
        struct RzArr {
            llvm::Value *base = nullptr;           // The region base.
            llvm::Value *len = nullptr;            // The element count.
            llvm::GlobalVariable *slot = nullptr;  // The gs_stack { top } owner.
            TypeExpr *etype = nullptr;             // The element type.
        };
        std::unordered_map<VarDef *, RzArr> rzarrs;
        std::vector<Loop> loops;
    };

    // Number of live specializations per qualified Goose function name. The
    // C backend suffixes every overloaded or multiply-specialized name.
    std::unordered_map<string_view, int> name_counts;
    // Global i64 variables: thread-local LLVM global values, initialized
    // by a gs_globals_init function at program start (M3 scope).
    struct GlobalVar {
        llvm::GlobalVariable *gv = nullptr;
        VarDef *vdef = nullptr;
    };
    std::unordered_map<VarDef *, GlobalVar> global_vars;

    // The i64 types a walk accepts: i8..i64/u8..u64 all as i64, for now.
    static bool is_int_ty(TypeExpr *t) {
        return t && t->kind == TY_INT && t->intstorage != IS_VARINT;
    }

    // A growable array of an int-walkable element: the data-stack slice's
    // subject. A_GROWSHRINK stays out for now: shrinking from the front
    // moves gs_rhdr.base, which this slice does not lower.
    static bool is_growable_i64(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && t->arr->akind == A_GROW
            && is_int_ty(t->arr->sub);
    }

    // An int type's LLVM storage width: u8 is i8, and so on.
    llvm::Type *int_ll_ty(TypeExpr *t) {
        switch (t->intstorage) {
            case IS_I8: case IS_U8:   return llvm::Type::getInt8Ty(ctx);
            case IS_I16: case IS_U16: return llvm::Type::getInt16Ty(ctx);
            case IS_I32: case IS_U32: return llvm::Type::getInt32Ty(ctx);
            default:                  return llvm::Type::getInt64Ty(ctx);
        }
    }

    static bool is_uint_ty(TypeExpr *t) {
        return t && t->kind == TY_INT
            && (t->intstorage == IS_U8 || t->intstorage == IS_U16
                || t->intstorage == IS_U32 || t->intstorage == IS_U64);
    }

    // A growable-array global (var a: T[>..] top level): its header is
    // thread-local globals (base, len) next to the same slot shape, and
    // gs_globals_init sets them from the reserved region (int main()
    // reserves before calling it). A global pushed from any body and cast
    // checks against its count read it.
    std::unordered_map<VarDef *, FnWalk::RzArr> global_rzs;

    // A growable array's header by name holder: a local of this walk or a
    // program global. Null where the name is neither.
    FnWalk::RzArr *rz_head(FnWalk &w, VarDef *d) {
        auto lit = w.rzarrs.find(d);
        if (lit != w.rzarrs.end()) return &lit->second;
        auto git = global_rzs.find(d);
        if (git != global_rzs.end()) return &git->second;
        return nullptr;
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

        // Pass 0: create the global i64 variables as thread-local LLVM
        // global values, and their gs_globals_init initializer (M3 scope).
        emit_globals();
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

    // The runtime slot's top tracks the next free byte: past the len
    // elements (GEP by the element type is the byte arithmetic).
    void rz_sync_slot(FnWalk::RzArr &r, llvm::Value *basev, llvm::Value *lenv) {
        auto *et = int_ll_ty(r.etype);
        b.CreateStore(b.CreateInBoundsGEP(et, basev, lenv, "rztop"), r.slot);
    }

    // An i64-walked value into an element slot, narrowed to the element's
    // storage (a u8 push's 300 arrives as 44, the int conversion mod 2^8).
    void rz_store_elem(FnWalk::RzArr &r, llvm::Value *basev, llvm::Value *idx,
                       llvm::Value *v) {
        auto *et = int_ll_ty(r.etype);
        auto *ev = et == i64() ? v : b.CreateTrunc(v, et);
        b.CreateStore(ev, b.CreateInBoundsGEP(et, basev, idx));
    }

    // An element read widened back to the walk's i64 values: unsigned
    // element storage zero-extends, signed sign-extends.
    llvm::Value *rz_load_elem(FnWalk::RzArr &r, llvm::Value *basev,
                              llvm::Value *idx, const llvm::Twine &name) {
        auto *et = int_ll_ty(r.etype);
        auto *v = b.CreateLoad(et, b.CreateInBoundsGEP(et, basev, idx), name);
        if (et == i64()) return v;
        return is_uint_ty(r.etype) ? b.CreateZExt(v, i64()) : b.CreateSExt(v, i64());
    }

    // An int-to-int `as` cast: when the source's range can exceed the
    // target's, the value is range-checked (the GS_RANGE semantics of
    // runtime_inline.h:961) and a failure is a call to the exported
    // noreturn gs_asfail_i (the same runtime action the C backend's
    // range macro takes). An `as!` skips the check. The walk keeps its
    // uniform i64 value: truncated to the target storage, then widened
    // back, so the value is the low bits that storage holds. Index
    // (`u32 in arr`) and bound targets, and u64 sources, land with the
    // bound-type slice and are left unwalked for now.
    llvm::Value *GenAsCast(AsCast *ac, FnWalk &w) {
        auto *st = ac->child->exprtype;
        if (!st || st->kind != TY_INT || st->intstorage == IS_U64) return nullptr;
        auto *c = GenExpr(ac->child, w);
        if (!c) return nullptr;
        // An index or bound type (`u32 in arr`, `u32 upto arr`): check the
        // value against the array's live count in every build (the C
        // backend's IndexTypeCheck, a fail is gs_idxfail), then narrow as
        // any `as` does. The array the checker named may be a local of
        // this walk or a program global; anything else is an unwalked cut.
        if (ac->totype && ac->totype->ix) {
            if (!is_int_ty(ac->totype) || ac->totype->intstorage == IS_VARINT)
                return nullptr;
            auto *arr = rz_head(w, ac->totype->ix->arr);
            if (!arr) return nullptr;
            if (!ac->nobc) {
                auto *lenv = b.CreateLoad(i64(), arr->len);
                auto *okbb = llvm::BasicBlock::Create(ctx, "ixok", w.fn);
                auto *failbb = llvm::BasicBlock::Create(ctx, "ixfail", w.fn);
                auto *inr = ac->totype->ix->upto
                    ? b.CreateICmpULE(c, lenv, "ixin")
                    : b.CreateICmpULT(c, lenv, "ixin");
                b.CreateCondBr(inr, okbb, failbb);
                b.SetInsertPoint(failbb);
                b.CreateCall(f_gs_idxfail, {
                    c, lenv,
                    llvm::ConstantPointerNull::get(i8ptr()),
                    llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(ctx), 0) });
                b.CreateUnreachable();
                b.SetInsertPoint(okbb);
            }
            auto *et = int_ll_ty(ac->totype);
            if (et == i64()) return c;
            auto *narrow = b.CreateTrunc(c, et);
            return is_uint_ty(ac->totype) ? b.CreateZExt(narrow, i64())
                                          : b.CreateSExt(narrow, i64());
        }
        if (!ac->totype || !is_int_ty(ac->totype)
            || ac->totype->intstorage == IS_VARINT)
            return nullptr;
        auto tt = ac->totype->intstorage;
        auto [slo, shi] = IntRange(st->intstorage);
        auto [lo, hi] = IntRange(tt);
        if (!ac->unchecked && (slo < lo || shi > hi)) {
            auto *one32 = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(ctx), 0);
            auto *nullptrv = llvm::ConstantPointerNull::get(i8ptr());
            auto *okbb = llvm::BasicBlock::Create(ctx, "asok", w.fn);
            auto *failbb = llvm::BasicBlock::Create(ctx, "asfail", w.fn);
            auto *lobr = b.CreateICmpSGE(c, llvm::ConstantInt::get(i64(), (uint64_t)lo));
            auto *hibr = b.CreateICmpSLE(c, llvm::ConstantInt::get(i64(), (uint64_t)hi));
            auto *inr = b.CreateAnd(lobr, hibr, "inrange");
            b.CreateCondBr(inr, okbb, failbb);
            b.SetInsertPoint(failbb);
            b.CreateCall(f_gs_asfail_i, {
                b.CreateGlobalStringPtr("out of range"),
                c,
                b.CreateGlobalStringPtr(IntStorageName(tt)),
                nullptrv, one32 });
            b.CreateUnreachable();
            b.SetInsertPoint(okbb);
        }
        auto *et = int_ll_ty(ac->totype);
        if (et == i64()) return c;
        auto *narrow = b.CreateTrunc(c, et);
        return is_uint_ty(ac->totype) ? b.CreateZExt(narrow, i64())
                                      : b.CreateSExt(narrow, i64());
    }

    // A growable array local (`var a: i64[>..] = [...]`): its storage is one
    // dedicated data stack. The slot is a lazy thread-local gs_stack { top
    // } owned by this slice; emit_program_entry reserves each slot's
    // region through gs_reserve_region and stores the base into the slot
    // before goose_main runs. One activation per slot: reentering the
    // decl (recursion, a loop body) reuses the region honestly only with
    // the full gs_sp machinery, which lands with the M3 completion.
    void emit_growable_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        // This slice reads only literal construction; a copied array or a
        // call giving its source is a later cut.
        auto *al = i < vd->inits.size() && vd->inits[i]
            ? Is<ArrayLit>(vd->inits[i]) : nullptr;
        if (i < vd->inits.size() && vd->inits[i] && !al) return;
        string sname = cat("gs_stk", stk_slots.size());
        auto *slot = new llvm::GlobalVariable(
            *mod, i8ptr(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(i8ptr()),
            sname, nullptr, llvm::GlobalValue::GeneralDynamicTLSModel);
        stk_slots.push_back(slot);
        // The header: gs_rhdr { base, len }. After main's reservation the
        // slot reads the region base.
        auto *basea = b.CreateAlloca(i8ptr(), nullptr, llvm::Twine(def.name, ".base"));
        auto *lena = b.CreateAlloca(i64(), nullptr, llvm::Twine(def.name, ".len"));
        auto *basev = b.CreateLoad(i8ptr(), slot, llvm::Twine(def.name, ".top"));
        b.CreateStore(basev, basea);
        b.CreateStore(llvm::ConstantInt::get(i64(), 0), lena);
        auto &r = w.rzarrs[&def];
        r = { basea, lena, slot, def.type->arr->sub };
        // The literal's elements land in sequence; the fill form freezes
        // its value once (the C backend's FreezeFill contract,
        // codegen_construct.h:955) and repeats it in a loop.
        if (al && !al->elems.empty()) {
            for (size_t k = 0; k < al->elems.size(); ++k) {
                auto *ev = GenExpr(al->elems[k], w);
                if (!ev) return;
                rz_store_elem(r, basev, llvm::ConstantInt::get(i64(), k), ev);
            }
            auto *n = llvm::ConstantInt::get(i64(), al->elems.size());
            b.CreateStore(n, lena);
            rz_sync_slot(r, basev, n);
        } else if (al && al->fillval) {
            auto *fv = GenExpr(al->fillval, w);
            auto *n = al->fillcount ? GenExpr(al->fillcount, w) : nullptr;
            if (!fv || !n) return;
            auto *prebb = b.GetInsertBlock();
            auto *condbb = llvm::BasicBlock::Create(ctx, "fillcond", w.fn);
            auto *bodybb = llvm::BasicBlock::Create(ctx, "fillbody", w.fn);
            auto *donebb = llvm::BasicBlock::Create(ctx, "filldone", w.fn);
            b.CreateBr(condbb);
            b.SetInsertPoint(condbb);
            auto *fi = b.CreatePHI(i64(), 2, "fi");
            fi->addIncoming(llvm::ConstantInt::get(i64(), 0), prebb);
            b.CreateCondBr(b.CreateICmpULT(fi, n, "fillmore"), bodybb, donebb);
            b.SetInsertPoint(bodybb);
            rz_store_elem(r, basev, fi, fv);
            auto *next = b.CreateAdd(fi, llvm::ConstantInt::get(i64(), 1), "fi+1");
            fi->addIncoming(next, bodybb);
            b.CreateBr(condbb);
            b.SetInsertPoint(donebb);
            b.CreateStore(n, lena);
            rz_sync_slot(r, basev, n);
        }
        // An empty literal (`[]`, or `[..cap]`'s hint, whose capacity the
        // monotonic region model takes as given): len stays 0.
    }
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
        if (auto *ac = Is<AsCast>(n)) return GenAsCast(ac, w);
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
                auto git = global_vars.find(id->vdef);
                if (git != global_vars.end())
                    return b.CreateLoad(i64(), git->second.gv, llvm::Twine(id->vdef->name));
                auto lit = w.locals.find(id->vdef);
                if (lit != w.locals.end())
                    return b.CreateLoad(i64(), lit->second, llvm::Twine(id->vdef->name));
            }
            return nullptr;
        }
        if (auto *dt = Is<Dot>(n)) {
            // A member expression: this slice lowers `.len` (member is a
            // BuiltinKind, ast.h) on a growable i64 array local.
            if (dt->member == B_LEN) {
                auto *obj = Is<Ident>(dt->obj);
                if (obj && obj->vdef) {
                    if (auto *r = rz_head(w, obj->vdef))
                        return b.CreateLoad(i64(), r->len,
                                            llvm::Twine(obj->vdef->name, ".lenv"));
                }
            }
            return nullptr;
        }
        if (auto *ix = Is<Index>(n)) {
            // An element read a[i] on a growable i64 array local: bounds
            // check, then load base[i]. The comparison is unsigned, so a
            // negative i64 index is caught by the same check.
            auto *obj = Is<Ident>(ix->obj);
            if (obj && obj->vdef) {
                if (auto *r = rz_head(w, obj->vdef)) {
                    auto *idxv = GenExpr(ix->idx, w);
                    if (!idxv) return nullptr;
                    auto *basev = b.CreateLoad(i8ptr(), r->base);
                    auto *lenv = b.CreateLoad(i64(), r->len);
                    if (!ix->nobc) {
                        // The failside is noreturn (gs_idxfail), so the
                        // downstream emission lives in the pass block.
                        auto *okbb = llvm::BasicBlock::Create(ctx, "idxok", w.fn);
                        auto *failbb = llvm::BasicBlock::Create(ctx, "idxfail", w.fn);
                        b.CreateCondBr(b.CreateICmpULT(idxv, lenv, "ixok"), okbb, failbb);
                        b.SetInsertPoint(failbb);
                        b.CreateCall(f_gs_idxfail, {
                            idxv, lenv,
                            llvm::ConstantPointerNull::get(i8ptr()),
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(ctx), 0) });
                        b.CreateUnreachable();
                        b.SetInsertPoint(okbb);
                    }
                    return rz_load_elem(*r, basev, idxv, "rzip");
                }
            }
            return nullptr;
        }
        if (auto *c = Is<Call>(n)) {
            // push (memberof a.f spelling: the receiver is the callee's
            // Dot obj, args holds only the value) on a growable i64 array
            // local: store the element at base+len, bump len, and keep the
            // runtime slot's top at the next free byte (the contract of
            // the runtime's section 1.2).
            if (c->builtin == B_PUSH && c->args.size() == 1) {
                auto *rd = Is<Dot>(c->callee);
                auto *recv = rd ? Is<Ident>(rd->obj) : nullptr;
                if (recv && recv->vdef) {
                    auto *rit = rz_head(w, recv->vdef);
                    if (rit) {
                        auto *v = GenExpr(c->args[0], w);
                        if (!v) return nullptr;
                        auto *r = rit;
                        auto *basev = b.CreateLoad(i8ptr(), r->base, "rzbase");
                        auto *lenv = b.CreateLoad(i64(), r->len, "rzlen");
                        rz_store_elem(*r, basev, lenv, v);
                        auto *nlen = b.CreateAdd(
                            lenv, llvm::ConstantInt::get(i64(), 1), "rzlen+1");
                        b.CreateStore(nlen, r->len);
                        rz_sync_slot(*r, basev, nlen);
                        return nullptr;
                    }
                }
            }
            // print(i64) via the libgoose_rt.a stub.
            if (c->builtin == B_PRINT && c->args.size() == 1) {
                auto *arg = GenExpr(c->args[0], w);
                if (arg) b.CreateCall(f_gs_print_i64, { arg });
                return nullptr;
            }
            // A direct call to a Goose function: all i64 args, i64 ret,
            // and the valueless (void) call whose effect the statement
            // walk keeps.
            if (c->spec) {
                auto *retty = voidty();
                if (c->spec->rets.size() == 1) {
                    retty = is_int_ty(c->spec->rets[0]) ? i64() : nullptr;
                } else if (!c->spec->rets.empty()) {
                    retty = nullptr;
                }
                for (auto *t : c->spec->argtypes)
                    if (!is_int_ty(t)) retty = nullptr;
                if (!retty) return nullptr;
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
                // The parser parked a void function's ending here as the
                // value form (a nested block's ending mark); the checker
                // proved it produces on no path, so it stays a statement
                // (typecheck_calls.h: a void function may end in one).
                if (top && w.fn->getReturnType()->isVoidTy()) {
                    GenStmt(blk->tail, w);
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
            // Growable i64 arrays: a dedicated data stack slot + header.
            // Scalars keep the alloca + store path below. A global growable
            // array is not a local stack slot; later cut.
            for (size_t i = 0; i < vd->defs.size(); ++i) {
                auto *vddef = vd->defs[i];
                if (!vddef) continue;
                if (vd->isglobal) continue;
                if (is_growable_i64(vddef->type)) {
                    emit_growable_decl(vd, i, *vddef, w);
                    continue;
                }
                if (!is_int_ty(vddef->type)) continue;
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
                auto gvit = global_vars.find(lv->vdef);
                if (gvit != global_vars.end()) {
                    cur = b.CreateLoad(i64(), gvit->second.gv, llvm::Twine(lv->vdef->name));
                } else {
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
            auto gv2 = global_vars.find(lv->vdef);
            if (gv2 != global_vars.end()) {
                b.CreateStore(rhs, gv2->second.gv);
            } else if (pit2 != w.params.end()) {
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
            w.loops.push_back({condbb, donebb});
            b.CreateBr(condbb);
            b.SetInsertPoint(condbb);
            auto *cond = GenExpr(wh->cond, w);
            if (!cond) {
                w.loops.pop_back();
                return;
            }
            if (!cond->getType()->isIntegerTy(1)) {
                auto *z = llvm::ConstantInt::get(i64(), 0);
                cond = b.CreateICmpNE(cond, z, "while_test");
            }
            b.CreateCondBr(cond, bodybb, donebb);
            b.SetInsertPoint(bodybb);
            if (wh->body) GenStmt(wh->body, w);
            if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(condbb);
            w.loops.pop_back();
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
                w.loops.push_back({condbb, donebb});
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
                w.loops.pop_back();
                b.SetInsertPoint(donebb);
                return;
            }
            return;
        }
        if (auto *brk = Is<Break>(n)) {
            if (!w.loops.empty()) {
                // A valueless `break` out of the innermost loop. A valued
                // break belongs to a `loop` expression (LoopExpr), which is
                // M3 work: its value would flow out of the loop, so the
                // minimal walk leaves it to the verification pass.
                if (brk->val) return;
                b.CreateBr(w.loops.back().donebb);
                return;
            }
            // A `break` out of an EarlyBlock: its value (or the block's
            // fall-through) is the block's result, handed up as the flow-out.
            w.flowout = brk->val ? GenExpr(brk->val, w) : nullptr;
            return;
        }
        if (auto *cont = Is<Continue>(n)) {
            if (w.loops.empty()) return;
            b.CreateBr(w.loops.back().condbb);
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
        auto *i32ty = llvm::Type::getInt32Ty(ctx);

        // C entry: int main(argc, argv) -> the Goose main (renamed
        // `goose_main` to avoid the link collision with the C runtime's
        // own symbol). The signature is uniform across IR programs.
        auto *mfn = mod->getFunction("goose_main");
        auto *mfnty = llvm::FunctionType::get(i32ty, { i32ty, i8ptr() }, false);
        auto *mfn2 = llvm::Function::Create(mfnty, llvm::GlobalValue::ExternalLinkage,
                                            "main", mod.get());
        auto *mentry = llvm::BasicBlock::Create(ctx, "entry", mfn2);
        b.SetInsertPoint(mentry);
        auto *argc = mfn2->getArg(0);
        auto *argv = mfn2->getArg(1);
        // Data stacks: start the runtime, then reserve one region per
        // growable-array slot. gs_reserve_region returns the base without
        // touching stack->top (runtime_impl.h:642), so this entry stores
        // it, the same contract gs_stack_init keeps for its caller. One
        // region per slot is where the runtime's gs_regions registry
        // counts in; the counts come from the runtime's own constants.
        if (!stk_slots.empty()) {
            b.CreateCall(f_gs_rt_start, {
                argc, argv,
                llvm::ConstantInt::get(i64(), 2048ull << 20),   // GS_STACK_RESERVE
                llvm::ConstantInt::get(i64(), 1ull << 20),      // GS_STACK_GAP
                llvm::ConstantInt::get(i64(), 32ull << 40),     // GS_STACK_BUDGET
                llvm::ConstantInt::get(
                    i64(), (uint64_t)stk_slots.size(), /*issigned=*/true),
                llvm::ConstantInt::get(i64(), 0) });            // workerregions
            for (auto *slot : stk_slots) {
                auto *top = b.CreateCall(f_gs_reserve_region, { slot });
                b.CreateStore(top, slot);
            }
        }
        // Initialize the globals before the Goos main runs. Only call
        // gs_globals_init when emit_globals() gave it a body; a bare extern
        // declaration would be an undefined reference at link time.
        if (f_gs_globals_init && !f_gs_globals_init->empty())
            b.CreateCall(f_gs_globals_init, {});
        if (mfn) b.CreateCall(mfn, {});
        b.CreateRet(llvm::ConstantInt::get(i32ty, 0));
    }

    // Create the global i64 variables as thread-local LLVM global values and
    // emit a `gs_globals_init` function that stores their initializers into
    // them (called from `int main()` before `goose_main`). Initializers are
    // evaluated in the program-level scope, where no locals or parameters
    // are in flight; a literal or expression that the minimal walk cannot
    // emit simply leaves the global zero.
    void emit_globals() {
        auto i64ty = i64();
        vector<pair<VarDef *, Node *>> inits;
        for (auto *g : ast.globals) {
            for (size_t di = 0; di < g->defs.size(); ++di) {
                auto *d = g->defs[di];
                if (is_growable_i64(d->type)) {
                    // One slot per global in reservation order (globals go
                    // before body-walked locals), its header thread-local.
                    std::string gname(d->name);
                    auto *slot = new llvm::GlobalVariable(
                        *mod, i8ptr(), /*isConstant=*/false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(i8ptr()),
                        cat("gs_stk", stk_slots.size()), nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    stk_slots.push_back(slot);
                    auto *basegv = new llvm::GlobalVariable(
                        *mod, i8ptr(), false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(i8ptr()),
                        gname + ".base", nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    auto *lengv = new llvm::GlobalVariable(
                        *mod, i64ty, false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantInt::get(i64ty, 0),
                        gname + ".len", nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    global_rzs[d] = { basegv, lengv, slot, d->type->arr->sub };
                    continue;
                }
                if (!is_int_ty(d->type)) continue;
                // A string_view's .data() is not null-terminated, so the name
                // must be copied into a std::string (passing .data() directly
                // makes LLVM read to the end of the source buffer).
                std::string gname(d->name);
                auto *gv = new llvm::GlobalVariable(
                    *mod, i64ty, false,
                    llvm::GlobalValue::InternalLinkage,
                    llvm::ConstantInt::get(i64ty, 0),
                    gname, nullptr,
                    llvm::GlobalValue::GeneralDynamicTLSModel);
                global_vars[d] = { gv, d };
                if (di < g->inits.size() && g->inits[di])
                    inits.push_back({ d, g->inits[di] });
            }
        }
        if (inits.empty() && global_rzs.empty()) return;
        // Reuse the extern declaration from declare_entry_externs() and give
        // it a body. A second Function::Create with the same name would be
        // renamed gs_globals_init.1, leaving the extern undefined at link.
        auto *gfn = f_gs_globals_init;
        auto *gbb = llvm::BasicBlock::Create(ctx, "entry", gfn);
        b.SetInsertPoint(gbb);
        FnWalk gw;
        gw.fn = gfn;
        for (auto &[d, ninit] : inits) {
            auto *initv = GenExpr(ninit, gw);
            if (initv) b.CreateStore(initv, global_vars[d].gv);
        }
        // A growable global's header: base from its reserved region, count
        // zero. The initializer this slice takes is [] only, whose contents
        // are nothing to store.
        for (auto &[d, r] : global_rzs) {
            b.CreateStore(b.CreateLoad(i8ptr(), r.slot, "gbase"), r.base);
            b.CreateStore(llvm::ConstantInt::get(i64(), 0), r.len);
        }
        b.CreateRetVoid();
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
