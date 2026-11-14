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
#include <deque>
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

    // The first construct the walk could not emit, so the driver refuses the
    // program instead of emitting one that silently lacks it.
    string unsupported;
    void Unsup(const string &what) {
        if (unsupported.empty()) unsupported = what;
    }
    const string &Unsupported() const { return unsupported; }

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
    llvm::Function *f_gs_divfail = nullptr;          // (ptr,i32), noreturn
    llvm::Function *f_gs_divovf = nullptr;           // (ptr,i32), noreturn
    llvm::Function *f_gs_abort = nullptr;            // (i32,ptr,i32), noreturn
    llvm::Function *f_gs_fmt_i64 = nullptr;          // (ptr,i64) -> i64 appended
    llvm::Function *f_gs_fmt_u64 = nullptr;          // (ptr,i64) -> i64 appended
    llvm::Function *f_gs_fmt_bool = nullptr;         // (ptr,i64) -> i64 appended
    llvm::Function *f_gs_out_bytes = nullptr;        // (ptr,i64)

    // The print line's scratch slot: one dedicated data-stack region the
    // line cursor runs over, reset per print (emit_program_entry reserves
    // it like every other slot).
    llvm::StructType *slice_ll_ty_ = nullptr;
    llvm::GlobalVariable *gs_pstk = nullptr;

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
        slice_ll_ty_ = llvm::StructType::get(ctx, { i8ptr(), i64() });
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
        dcl("gs_divfail", f_gs_divfail, V, { P, I32 });
        dcl("gs_divovf", f_gs_divovf, V, { P, I32 });
        dcl("gs_abort", f_gs_abort, V, { I32, P, I32 });
        dcl("gs_fmt_i64", f_gs_fmt_i64, I, { P, I });
        dcl("gs_fmt_u64", f_gs_fmt_u64, I, { P, I });
        dcl("gs_fmt_bool", f_gs_fmt_bool, I, { P, I });
        dcl("gs_out_bytes", f_gs_out_bytes, V, { P, I });
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
        // The spec being walked, and the index of its first out-pointer
        // parameter: results 1.. of a multi-return callee arrive there.
        FnSpec *spec = nullptr;
        size_t outstart = 0;
        // The walked body Block (only the function's own; nested blocks are
        // not top-level, so their value tails do not become rets).
        Block *body = nullptr;
        // Growable array locals of this walk: VarDef * -> header. base and
        // len are local allocas (their alloca'd header); the same record
        // shape also carries a global's thread-local header (storage
        // shared across bodies instead of a stack frame's).
        struct RzArr {
            llvm::Value *base = nullptr;           // The header's base field.
            llvm::Value *len = nullptr;            // The header's count field.
            llvm::Value *slot = nullptr;           // The gs_stack { top } owner.
            llvm::Value *hdr = nullptr;            // The { base, len } header.
            TypeExpr *etype = nullptr;             // The element type.
        };
        // The inlined bodies being walked, innermost last. A return whose
        // target is one of them branches to its merge block with the value.
        struct InlExit {
            SFunction *sf = nullptr;
            FnSpec *spec = nullptr;       // The inlined specialization.
            llvm::BasicBlock *merge = nullptr;
            std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
            // A growable single result's run destination: the local the
            // caller bound it to. Each inlined `return a` copies its run
            // there instead of flowing a value to the merge.
            FnWalk::RzArr *growret = nullptr;
        };
        std::vector<InlExit> inl;
        // The enclosing loops of the walk (innermost last); a break or
        // continue without a value targets the innermost one.
        struct Loop {
            llvm::BasicBlock *condbb = nullptr;   // The loop's condition block.
            llvm::BasicBlock *donebb = nullptr;    // The loop's exit block.
            llvm::BasicBlock *contbb = nullptr;    // Where `continue` resumes
                                                   // (the increment, if any).
        };
        std::unordered_map<VarDef *, RzArr> rzarrs;
        // Reference locals and parameters to a growable array: one holder
        // of { header pointer, stack pointer }, the pair the C backend's
        // gs_rref carries. Every use loads the pair, so a rebind is seen
        // there, and the header's fields stay the caller's own.
        struct RzRefLoc {
            llvm::AllocaInst *holder = nullptr;    // The { hdr, stk } pair.
            TypeExpr *etype = nullptr;             // The element type.
        };
        std::unordered_map<VarDef *, RzRefLoc> rzrefs;
        // Views materialized through a reference: one record per use, held
        // where its address stays stable for the site that asked for it.
        std::deque<RzArr> rzviews;
        // Fixed arrays (T[k]) of int elements walked in this body: the
        // element storage lands in one alloca of [count x elemty].
        struct FixedArr {
            llvm::AllocaInst *base = nullptr;      // The storage.
            int64_t count = -1;                    // Its element count.
            TypeExpr *etype = nullptr;             // The element type.
        };
        std::unordered_map<VarDef *, FixedArr> fixeds;
        // Limited arrays (T[..k]) of int elements walked in this body: the
        // static-capacity fixed-slot value (section 4.1). The slots are one
        // alloca of [cap x elemty] and the count one i64 alloca; the C
        // value form lays them together as one struct { len, e[k] }, which
        // a later ABI slice mirrors.
        struct LimArr {
            llvm::AllocaInst *base = nullptr;      // The element slots.
            llvm::AllocaInst *len = nullptr;       // The element count.
            int64_t cap = -1;                      // The declared capacity.
            TypeExpr *etype = nullptr;             // The element type.
        };
        std::unordered_map<VarDef *, LimArr> limiteds;
        // Reference locals and parameters (T& of an int): one alloca
        // holding the pointer the reference names. A parameter's stores
        // its argument, a declaration's the variable its initializer
        // names, and a rebind (`r .= e`) a new one; every read and write
        // of the reference loads the pointer and reaches the pointee.
        struct RefLoc {
            llvm::AllocaInst *base = nullptr;      // The pointer holder.
            TypeExpr *ptee = nullptr;              // The pointee's type.
        };
        std::unordered_map<VarDef *, RefLoc> refs;
        // Flat int-field struct locals: their storage and the struct's layout.
        struct StructLoc {
            llvm::AllocaInst *base = nullptr;
            StructInst *inst = nullptr;
        };
        std::unordered_map<VarDef *, StructLoc> structs;
        // Slice locals (T[:] of int): a { data, len } alloca and its element.
        struct SliceLoc {
            llvm::AllocaInst *base = nullptr;
            TypeExpr *etype = nullptr;
        };
        std::unordered_map<VarDef *, SliceLoc> slices;
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

    // A bool is walked in an i64 slot like the ints (0/1); comparisons
    // produce i1, so a value stored or passed as a scalar widens.
    static bool is_bool_ty(TypeExpr *t) { return t && t->kind == TY_BOOL; }

    llvm::Value *as_i64(llvm::Value *v) {
        if (v && v->getType()->isIntegerTy(1)) return b.CreateZExt(v, i64(), "widen");
        return v;
    }

    // A value stored into a variable of a narrow int type wraps at that
    // storage's width, as the C backend's store does: the low bits kept,
    // then widened back (unsigned zero, signed sign) into the walk's i64.
    llvm::Value *widen_store(TypeExpr *t, llvm::Value *v) {
        if (!t || t->kind != TY_INT || !v) return v;
        auto *st = int_ll_ty(t);
        if (st == i64() || v->getType() != i64()) return v;
        auto *tr = b.CreateTrunc(v, st, "narrow");
        return is_uint_ty(t) ? b.CreateZExt(tr, i64()) : b.CreateSExt(tr, i64());
    }

    // A reference to an i64, the one pointee width this slice gives a
    // function result: every narrower local lives widened in an i64 slot, so
    // its address is not the value's.
    bool is_i64_ref(TypeExpr *t) {
        return t && t->kind == TY_REF && t->ref->sub
            && t->ref->sub->kind == TY_INT && int_ll_ty(t->ref->sub) == i64();
    }

    // The flat struct a reference type points at, or null: the pointee a
    // path or a whole-value read reaches through the pointer.
    static TypeExpr *struct_ref_pointee(TypeExpr *t) {
        return t && t->kind == TY_REF && is_flat_struct(t->ref->sub)
             ? t->ref->sub : nullptr;
    }

    // A reference this slice carries as one pointer: its pointee is an i64
    // or a flat int struct, both of which live in an alloca of their own.
    // A growable pointee is the { header, stack } pair instead (rzrefs).
    bool is_flat_ref(TypeExpr *t) {
        return t && t->kind == TY_REF
            && (is_i64_ref(t) || is_flat_struct(t->ref->sub));
    }

    // A growable array of an int-walkable element: the data-stack slice's
    // subject. A_GROWSHRINK stays out for now: shrinking from the front
    // moves gs_rhdr.base, which this slice does not lower.
    static bool is_growable_i64(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && t->arr->akind == A_GROW
            && is_int_ty(t->arr->sub);
    }

    // A type's name for a refusal message.
    static string type_label(TypeExpr *t) {
        if (!t) return "an unknown type";
        switch (t->kind) {
            case TY_INT:   return "an int";
            case TY_FLT:   return "a float";
            case TY_BOOL:  return "a bool";
            case TY_VOID:  return "void";
            case TY_STRUCT:
                return t->struc && t->struc->st
                    ? cat("struct ", t->struc->st->name) : string("a struct");
            case TY_ENUM:
                return t->enu && t->enu->en
                    ? cat("enum ", t->enu->en->name) : string("an enum");
            case TY_VARIANT: return "a variant";
            case TY_ARRAY: {
                if (!t->arr) return "an array";
                const char *k = t->arr->akind == A_FIXED ? "a fixed array"
                              : t->arr->akind == A_VAR ? "a length-stored array"
                              : t->arr->akind == A_LIMITED ? "a limited array"
                              : t->arr->akind == A_GROWSHRINK ? "a grow-shrink array"
                              : "a growable array";
                return cat(k, " of ", type_label(t->arr->sub));
            }
            case TY_SLICE:  return cat("a slice of ", type_label(t->sub));
            case TY_REF:    return cat("a reference to ", type_label(t->ref ? t->ref->sub : nullptr));
            case TY_FN:     return "a function value";
            default:        return "a type";
        }
    }

    // A struct whose storage is plain: fixed size, flat, no padding, and
    // every field an int or another such struct (section 4.4 allows a field
    // of any type the struct holds inline). Its fields are in declaration
    // order, so a field index is its position in st->fields.
    static bool is_flat_struct(TypeExpr *t) {
        if (!t || t->kind != TY_STRUCT || !t->struc || !t->struc->inst) return false;
        auto *in = t->struc->inst;
        if (in->sclass != SC_FIXED || !in->flat || in->failed || !in->st) return false;
        if (in->st->fields.empty() || in->ftypes.size() != in->st->fields.size()) return false;
        for (size_t i = 0; i < in->st->fields.size(); ++i) {
            if (in->st->fields[i].ispad) return false;
            if (!is_int_ty(in->ftypes[i]) && !is_flat_struct(in->ftypes[i]))
                return false;
        }
        return true;
    }

    // A field's LLVM type: an int at its storage width, or a nested struct's
    // own record.
    llvm::Type *field_ll_ty(TypeExpr *ft) {
        return is_int_ty(ft) ? int_ll_ty(ft) : struct_ll_ty(ft->struc->inst);
    }

    // A slice the walk holds: a view of int elements, { data, len }.
    struct SliceVal {
        llvm::Value *data = nullptr;
        llvm::Value *len = nullptr;
        TypeExpr *etype = nullptr;
    };

    static bool is_slice_of_int(TypeExpr *t) {
        return t && t->kind == TY_SLICE && is_int_ty(t->sub);
    }

    llvm::StructType *slice_ll_ty() { return slice_ll_ty_; }

    // A reference to a growable array: the pair the C backend carries as
    // `gs_rref { gs_rhdr *hdr; gs_stack *stk; }` (codegen_types.h:510).
    llvm::StructType *rzref_ll_ty() {
        return llvm::StructType::get(ctx, { i8ptr(), i8ptr() });
    }

    // Constant GEP indices: the zero pointer step and a struct field
    // number (which the IR always spells i32).
    llvm::Constant *idx0() { return llvm::ConstantInt::get(i64(), 0); }
    llvm::Constant *fld(unsigned k) {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), k);
    }

    // A local's storage in the entry block: a declaration inside a loop
    // body would otherwise allocate on every pass and walk the native stack
    // off its end, where C gives the function one frame slot the scope
    // reuses.
    llvm::AllocaInst *entry_alloca(FnWalk &w, llvm::Type *ty,
                                   const llvm::Twine &name) {
        auto &entry = w.fn->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(ty, nullptr, name);
    }

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

    // A string literal's constant bytes: one internal constant global per
    // distinct literal, no terminator (the length travels beside it).
    std::unordered_map<std::string, llvm::GlobalVariable *> strlits;
    llvm::GlobalVariable *strlit_glob(const std::string &val) {
        auto it = strlits.find(val);
        if (it != strlits.end()) return it->second;
        llvm::SmallVector<uint8_t, 32> bytes(val.begin(), val.end());
        auto *arr = llvm::ConstantDataArray::get(ctx, bytes);
        auto *gv = new llvm::GlobalVariable(
            *mod, arr->getType(), /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, arr,
            cat("gs_str", strlits.size()));
        strlits.emplace(val, gv);
        return gv;
    }

    // A growable array's header by name holder: a local of this walk, a
    // program global, or a reference (whose view is materialized here, the
    // pair loaded fresh so a rebind is seen). Null where the name is none.
    FnWalk::RzArr *rz_head(FnWalk &w, VarDef *d) {
        auto lit = w.rzarrs.find(d);
        if (lit != w.rzarrs.end()) return &lit->second;
        auto git = global_rzs.find(d);
        if (git != global_rzs.end()) return &git->second;
        auto rit = w.rzrefs.find(d);
        if (rit != w.rzrefs.end()) {
            llvm::Value *hp = nullptr;
            llvm::Value *sp = nullptr;
            if (!growable_pair(w, d, hp, sp)) return nullptr;
            FnWalk::RzArr v;
            v.hdr = hp;
            v.slot = sp;
            v.etype = rit->second.etype;
            v.base = b.CreateInBoundsGEP(slice_ll_ty(), hp, { idx0(), fld(0) },
                                         "rzbase");
            v.len = b.CreateInBoundsGEP(slice_ll_ty(), hp, { idx0(), fld(1) },
                                        "rzlen");
            w.rzviews.push_back(v);
            return &w.rzviews.back();
        }
        return nullptr;
    }

    // The { header, stack } pair naming the growable array a name holds, or
    // a reference to one (its pair is loaded from the holder at this point
    // of the walk, so a rebind before it is seen). False where the walk
    // does not hold the name.
    bool growable_pair(FnWalk &w, VarDef *d, llvm::Value *&hdr,
                       llvm::Value *&stk) {
        auto lit = w.rzarrs.find(d);
        if (lit != w.rzarrs.end()) {
            hdr = lit->second.hdr;
            stk = lit->second.slot;
            return true;
        }
        auto git = global_rzs.find(d);
        if (git != global_rzs.end()) {
            hdr = git->second.hdr;
            stk = git->second.slot;
            return true;
        }
        auto rit = w.rzrefs.find(d);
        if (rit != w.rzrefs.end()) {
            auto *holder = rit->second.holder;
            hdr = b.CreateLoad(i8ptr(),
                               b.CreateStructGEP(rzref_ll_ty(), holder, 0),
                               "rzhdr");
            stk = b.CreateLoad(i8ptr(),
                               b.CreateStructGEP(rzref_ll_ty(), holder, 1),
                               "rzstk");
            return true;
        }
        return false;
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
                if (is_int_ty(t) || is_bool_ty(t)) argtys.push_back(i64ty);
                else if (t->kind == TY_REF && is_int_ty(t->ref->sub))
                    argtys.push_back(i8ptr());
                else if (t->kind == TY_REF && is_flat_struct(t->ref->sub))
                    argtys.push_back(i8ptr());
                else if (t->kind == TY_REF && is_growable_i64(t->ref->sub)) {
                    // A reference to a growable array: the C backend's
                    // (gs_rhdr *, gs_stack *) pair, two pointers.
                    argtys.push_back(i8ptr());
                    argtys.push_back(i8ptr());
                }
                else if (is_slice_of_int(t)) argtys.push_back(slice_ll_ty());
                else if (is_flat_struct(t)) argtys.push_back(struct_ty(t));
                else supported = false;
            }
            // Return protocol: ret0 is the call's value, every other
            // result an out-pointer parameter the callee writes; a single
            // slice result comes back as the { data, len } it is; a single
            // growable result is the run form: the callee receives the
            // caller's header-and-stack pair to build into (dst, rl).
            bool allretint = true;
            bool growret = sp->rets.size() == 1 && is_growable_i64(sp->rets[0]);
            for (auto *rt : sp->rets)
                if (!is_int_ty(rt) && !is_bool_ty(rt) && !is_flat_struct(rt))
                    allretint = false;
            llvm::Type *retty = voidty();
            if (growret) {
                // The run form: no value returns, the caller's (hdr, stack)
                // go in as the last two arguments.
                retty = voidty();
                argtys.push_back(i8ptr());   // dst: the caller's header base
                argtys.push_back(i8ptr());   // rl: the caller's count
            } else if (!sp->rets.empty()) {
                if (sp->rets.size() == 1 && is_slice_of_int(sp->rets[0]))
                    retty = slice_ll_ty();
                else if (sp->rets.size() == 1 && is_flat_struct(sp->rets[0]))
                    retty = struct_ty(sp->rets[0]);
                else if (sp->rets.size() == 1 && is_flat_ref(sp->rets[0]))
                    retty = i8ptr();
                else if ((is_int_ty(sp->rets[0]) || is_bool_ty(sp->rets[0]))
                         && allretint)
                    retty = i64ty;
                else
                    retty = nullptr;
                if (!retty) supported = false;
                for (size_t i = 1; i < sp->rets.size(); i++)
                    argtys.push_back(i8ptr());
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
            w.spec = sp;
            w.outstart = sp->argtypes.size();
            w.body = sp->body;
            w.entry = llvm::BasicBlock::Create(ctx, "entry", fn);
            b.SetInsertPoint(w.entry);
            // Bind parameters (aligned with argtypes): ints as i64 slots, a
            // slice into an alloca so the body may re-point it. A reference
            // to a growable array takes two arguments, so the argument
            // index advances by what each parameter consumed.
            size_t argidx = 0;
            for (size_t i = 0; i < sp->params.size()
                 && argidx < fn->getFunctionType()->getNumParams(); ++i) {
                auto *pt = i < sp->argtypes.size() ? sp->argtypes[i] : nullptr;
                if (pt && pt->kind == TY_REF && is_growable_i64(pt->ref->sub)) {
                    // The pair of pointers the caller passed, kept in one
                    // holder the body reads; a rebind rewrites it.
                    auto *holder = b.CreateAlloca(rzref_ll_ty(), nullptr,
                                                  llvm::Twine(sp->params[i]->name));
                    b.CreateStore(fn->getArg(argidx),
                                  b.CreateStructGEP(rzref_ll_ty(), holder, 0));
                    b.CreateStore(fn->getArg(argidx + 1),
                                  b.CreateStructGEP(rzref_ll_ty(), holder, 1));
                    w.rzrefs[sp->params[i]] = { holder, pt->ref->sub->arr->sub };
                    argidx += 2;
                } else if (is_slice_of_int(pt)) {
                    auto *slot = b.CreateAlloca(slice_ll_ty(), nullptr,
                                                llvm::Twine(sp->params[i]->name));
                    b.CreateStore(fn->getArg(argidx), slot);
                    w.slices[sp->params[i]] = { slot, pt->sub };
                    argidx += 1;
                } else if (pt->kind == TY_REF
                           && (is_int_ty(pt->ref->sub)
                               || is_flat_struct(pt->ref->sub))) {
                    // A reference parameter: one pointer holder, its store
                    // the argument's slot; the body's rebinds write there.
                    auto *slot = b.CreateAlloca(i8ptr(), nullptr,
                                                llvm::Twine(sp->params[i]->name));
                    b.CreateStore(fn->getArg(argidx), slot);
                    w.refs[sp->params[i]] = { slot, pt->ref->sub };
                    argidx += 1;
                } else if (is_flat_struct(pt)) {
                    auto *slot = b.CreateAlloca(struct_ty(pt), nullptr,
                                                llvm::Twine(sp->params[i]->name));
                    b.CreateStore(fn->getArg(argidx), slot);
                    w.structs[sp->params[i]] = { slot, pt->struc->inst };
                    argidx += 1;
                } else if (!pt || is_int_ty(pt) || is_bool_ty(pt)) {
                    // An int parameter lives in a slot of the body's own from
                    // the start: a loop that reassigns it each round (the
                    // tail-recursion form) then reads what the last round
                    // stored, not the value the call arrived with.
                    auto *slot = entry_alloca(w, i64(),
                                             llvm::Twine(sp->params[i]->name));
                    b.CreateStore(widen_store(pt, fn->getArg(argidx)), slot);
                    w.locals[sp->params[i]] = slot;
                    argidx += 1;
                }
            }
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
                    else if (fn->getReturnType() == slice_ll_ty())
                        b.CreateRet(llvm::Constant::getNullValue(slice_ll_ty()));
                    else if (fn->getReturnType()->isStructTy())
                        b.CreateRet(llvm::Constant::getNullValue(
                            fn->getReturnType()));
                    else if (fn->getReturnType()->isPointerTy())
                        b.CreateRet(llvm::ConstantPointerNull::get(i8ptr()));
                    else b.CreateRet(llvm::ConstantInt::get(i64ty, 0));
                }
            }
        }

        emit_program_entry();
    }

    // ------------------------------------------------------------------
    // Body-walk definitions (minimal M3 subset, i64 values).
    // ------------------------------------------------------------------

    // The run-form destination of a growable result: dst points at the
    // caller's stack slot (its top), rl at the caller's count. The held
    // run's elements are copied there, the top advanced past them and the
    // count written.
    void emit_growable_into(FnWalk::RzArr *r, llvm::Value *dst,
                            llvm::Value *rl, FnWalk &w) {
        auto *basev = b.CreateLoad(i8ptr(), r->base, "rrun");
        auto *lenv = b.CreateLoad(i64(), r->len, "rrunlen");
        auto *stk = b.CreateLoad(i8ptr(), dst, "rdst");
        auto *et = int_ll_ty(r->etype);
        auto esz = et->getPrimitiveSizeInBits() / 8;
        if (esz) {
            auto *sz = llvm::ConstantInt::get(i64(), (uint64_t)esz);
            auto *nbytes = b.CreateMul(lenv, sz, "rbytes");
            if (nbytes != llvm::ConstantInt::get(i64(), 0))
                b.CreateMemCpy(stk, llvm::MaybeAlign(1), basev,
                               llvm::MaybeAlign(1), nbytes);
            b.CreateStore(
                b.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx), stk, nbytes,
                                    "rdstop"),
                dst);
        } else {
            b.CreateStore(stk, dst);
        }
        b.CreateStore(lenv, rl);
    }

    // Build a growable result's value at the run destination (dst, rl):
    // a name copies the run it holds, a literal or string literal is built
    // in place, and a slice view is copied over. False when the walk does
    // not build the value.
    bool emit_growable_result(Node *val, llvm::Value *dst, llvm::Value *rl,
                              TypeExpr *rettype, FnWalk &w) {
        if (auto *id = Is<Ident>(val)) {
            auto *r = id->vdef ? rz_head(w, id->vdef) : nullptr;
            if (r) { emit_growable_into(r, dst, rl, w); return true; }
        }
        auto *sub = rettype->arr->sub;
        auto *et = int_ll_ty(sub);
        auto esz = et->getPrimitiveSizeInBits() / 8;
        auto *i8ty = llvm::Type::getInt8Ty(ctx);
        auto *stk = b.CreateLoad(i8ptr(), dst, "rdst");
        if (auto *sl = Is<StrLit>(val)) {
            if (sub->intstorage != IS_U8 && sub->intstorage != IS_I8)
                return false;
            auto n = (uint64_t)sl->val.size();
            if (n)
                b.CreateMemCpy(stk, llvm::MaybeAlign(1), strlit_glob(sl->val),
                               llvm::MaybeAlign(1), n);
            b.CreateStore(b.CreateInBoundsGEP(i8ty, stk,
                llvm::ConstantInt::get(i64(), n), "rdstop"), dst);
            b.CreateStore(llvm::ConstantInt::get(i64(), n), rl);
            return true;
        }
        if (auto *al = Is<ArrayLit>(val)) {
            if (al->fillval) return false;   // A fill's count is a later cut.
            int64_t k = 0;
            for (auto *e : al->elems) {
                auto *v = GenExpr(e, w);
                if (!v) return false;
                b.CreateStore(et == i64() ? v : b.CreateTrunc(v, et),
                              b.CreateInBoundsGEP(et, stk,
                                  llvm::ConstantInt::get(i64(), (uint64_t)k),
                                  "rlitp"));
                k++;
            }
            b.CreateStore(b.CreateInBoundsGEP(et, stk,
                llvm::ConstantInt::get(i64(), (uint64_t)k), "rdstop"), dst);
            b.CreateStore(llvm::ConstantInt::get(i64(), (uint64_t)k), rl);
            return true;
        }
        SliceVal sv;
        if (slice_value(val, w, sv) && sv.etype) {
            auto *nbytes = b.CreateMul(sv.len,
                llvm::ConstantInt::get(i64(), (uint64_t)esz), "rbytes");
            b.CreateMemCpy(stk, llvm::MaybeAlign(1), sv.data,
                           llvm::MaybeAlign(1), nbytes);
            b.CreateStore(b.CreateInBoundsGEP(i8ty, stk, nbytes, "rdstop"),
                          dst);
            b.CreateStore(sv.len, rl);
            return true;
        }
        return false;
    }

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
        if (!st || st->kind != TY_INT) return nullptr;
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
        // u64 is the one source whose values exceed the int64 range the
        // signed checks compute in: its narrowing compares unsigned (the
        // C backend's GS_RANGE_U), and a u64 target needs no check.
        bool su64 = st->intstorage == IS_U64;
        auto [slo, shi] = IntRange(st->intstorage);
        auto [lo, hi] = IntRange(tt);
        bool need = !ac->unchecked && (su64 ? tt != IS_U64
                                            : (slo < lo || shi > hi));
        if (need) {
            auto *one32 = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(ctx), 0);
            auto *nullptrv = llvm::ConstantPointerNull::get(i8ptr());
            auto *okbb = llvm::BasicBlock::Create(ctx, "asok", w.fn);
            auto *failbb = llvm::BasicBlock::Create(ctx, "asfail", w.fn);
            llvm::Value *inr = nullptr;
            if (su64) {
                inr = b.CreateICmpULE(c,
                    llvm::ConstantInt::get(i64(), (uint64_t)hi), "inrange");
            } else {
                auto *lobr = b.CreateICmpSGE(c, llvm::ConstantInt::get(i64(), (uint64_t)lo));
                auto *hibr = b.CreateICmpSLE(c, llvm::ConstantInt::get(i64(), (uint64_t)hi));
                inr = b.CreateAnd(lobr, hibr, "inrange");
            }
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

    // A fixed array local (let a: i64[k] / inferred from [..; k]): the
    // value is an alloca of [k x elemty], zero-filled first (the C
    // zeroes its literal temps), then the literal's construction: per
    // element stores, or the fill's value evaluated once (the
    // FreezeFill contract) and stored to every index unrolled.
    void emit_fixed_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        auto *tal = def.type->arr;
        auto count = tal->size;
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        auto *al = init ? Is<ArrayLit>(init) : nullptr;
        // A T[] local (no capacity, so no pushes) has the count its literal
        // gives; a T[k] has its declared one. A u8[] built from a string
        // literal is the literal's bytes.
        auto *sl = init ? Is<StrLit>(init) : nullptr;
        bool strinit = sl && tal->sub
            && (tal->sub->intstorage == IS_U8 || tal->sub->intstorage == IS_I8);
        if (tal->akind == A_VAR) {
            if (al && !al->fillval) count = (int64_t)al->elems.size();
            else if (strinit) count = (int64_t)sl->val.size();
            else return;
        }
        if (count < 0) return;
        auto *et = int_ll_ty(tal->sub);
        auto *a = count
            ? entry_alloca(w, llvm::ArrayType::get(et, (uint64_t)count),
                           llvm::Twine(def.name))
            : nullptr;
        if (a && et->getScalarSizeInBits() % 8 == 0) {
            b.CreateMemSet(a, (llvm::Value *)b.getInt8(0),
                           (uint64_t)count * (et->getScalarSizeInBits() / 8),
                           llvm::Align(8));
        }
        w.fixeds[&def] = { a, count, tal->sub };
        if (!al && strinit) {
            if (count)
                b.CreateMemCpy(a, llvm::MaybeAlign(1), strlit_glob(sl->val),
                               llvm::MaybeAlign(1), (uint64_t)count);
            return;
        }
        if (init && !al) return;      // Literal construction only here.
        if (!al) return;
        auto elem = [&](int64_t k, llvm::Value *v) {
            auto *ev = et == i64() ? v : b.CreateTrunc(v, et);
            b.CreateStore(ev, b.CreateInBoundsGEP(a->getAllocatedType(), a,
                { llvm::ConstantInt::get(i64(), 0),
                  llvm::ConstantInt::get(i64(), k) }, "fkp"));
        };
        if (al->fillval) {
            auto *v = GenExpr(al->fillval, w);
            if (!v) return;
            for (int64_t k = 0; k < count; k++) elem(k, v);
        } else if (count) {
            for (size_t k = 0; k < (size_t)count && k < al->elems.size(); k++) {
                auto *v = GenExpr(al->elems[k], w);
                if (!v) return;
                elem((int64_t)k, v);
            }
        }
    }

    // A static-capacity limited array local (`var a: i64[..k]`): its
    // capacity is the declared constant, the slots one alloca, and the
    // count an i64 alloca that a literal (or a string literal's bytes for
    // a u8 one) sets. Construction by copy from another array's value is
    // a later cut, as is the runtime-capacity `T[..]` form.
    void emit_limited_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        auto *tal = def.type->arr;
        if (tal->size < 0)
            return Unsup(cat("a local of a runtime-capacity limited array "
                             "at line ", (long long)vd->line.line));
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        auto *al = init ? Is<ArrayLit>(init) : nullptr;
        auto *sl = init ? Is<StrLit>(init) : nullptr;
        auto *sub = tal->sub;
        bool strinit = sl && sub
            && (sub->intstorage == IS_U8 || sub->intstorage == IS_I8);
        if (init && !al && !strinit)
            return Unsup(cat("a limited array constructed from a value the "
                             "walk does not build at line ",
                             (long long)vd->line.line));
        auto cap = tal->size;
        auto *et = int_ll_ty(sub);
        auto *a = entry_alloca(w, llvm::ArrayType::get(et, (uint64_t)cap),
                               llvm::Twine(def.name));
        auto *len = entry_alloca(w, i64(),
                                 llvm::Twine(def.name, ".len"));
        b.CreateMemSet(a, (llvm::Value *)b.getInt8(0),
                       (uint64_t)cap * (et->getScalarSizeInBits() / 8),
                       llvm::Align(8));
        w.limiteds[&def] = { a, len, cap, sub };
        if (strinit) {
            auto n = (uint64_t)sl->val.size();
            if (n > (uint64_t)cap) n = (uint64_t)cap;
            if (n)
                b.CreateMemCpy(a, llvm::MaybeAlign(1), strlit_glob(sl->val),
                               llvm::MaybeAlign(1), n);
            b.CreateStore(llvm::ConstantInt::get(i64(), n), len);
            return;
        }
        if (!al) return;
        auto count = CodeGen::LitCount(al);
        if (count > cap) count = cap;
        b.CreateStore(llvm::ConstantInt::get(i64(), (uint64_t)count), len);
        auto elem = [&](int64_t k, llvm::Value *v) {
            auto *ev = et == i64() ? v : b.CreateTrunc(v, et);
            b.CreateStore(ev, b.CreateInBoundsGEP(a->getAllocatedType(), a,
                { llvm::ConstantInt::get(i64(), 0),
                  llvm::ConstantInt::get(i64(), k) }, "fkp"));
        };
        if (al->fillval) {
            auto *v = GenExpr(al->fillval, w);
            if (!v) return;
            for (int64_t k = 0; k < count; k++) elem(k, v);
        } else {
            for (int64_t k = 0; k < count && (size_t)k < al->elems.size(); k++) {
                auto *v = GenExpr(al->elems[(size_t)k], w);
                if (!v) return;
                elem(k, v);
            }
        }
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
        // call giving its source is a later cut. A u8[>..] may start from a
        // string literal's bytes.
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        auto *al = init ? Is<ArrayLit>(init) : nullptr;
        auto *sl = init ? Is<StrLit>(init) : nullptr;
        auto *sub = def.type->arr->sub;
        bool strinit = sl && sub
            && (sub->intstorage == IS_U8 || sub->intstorage == IS_I8);
        bool runinit = (init && Is<Call>(init)) || (init && Is<InlineBlock>(init));
        if (init && !al && !strinit && !runinit) return;
        string sname = cat("gs_stk", stk_slots.size());
        auto *slot = new llvm::GlobalVariable(
            *mod, i8ptr(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(i8ptr()),
            sname, nullptr, llvm::GlobalValue::GeneralDynamicTLSModel);
        stk_slots.push_back(slot);
        // The header: gs_rhdr { base, len }. After main's reservation the
        // slot reads the region base.
        auto *hdra = entry_alloca(w, slice_ll_ty(),
                                  llvm::Twine(def.name, ".hdr"));
        auto *basea = b.CreateInBoundsGEP(slice_ll_ty(), hdra, { idx0(), fld(0) },
                                          llvm::Twine(def.name, ".base"));
        auto *lena = b.CreateInBoundsGEP(slice_ll_ty(), hdra, { idx0(), fld(1) },
                                         llvm::Twine(def.name, ".len"));
        auto *basev = b.CreateLoad(i8ptr(), slot, llvm::Twine(def.name, ".top"));
        b.CreateStore(basev, basea);
        b.CreateStore(llvm::ConstantInt::get(i64(), 0), lena);
        auto &r = w.rzarrs[&def];
        r = { basea, lena, slot, hdra, def.type->arr->sub };
        // A call's growable result is built straight into this local's
        // header and stack (the run form).
        if (auto *rc = init ? Is<Call>(init) : nullptr) {
            if (!rc->spec || rc->spec->rets.size() != 1
                || !is_growable_i64(rc->spec->rets[0])) {
                w.rzarrs.erase(&def);
                return Unsup(cat("a growable initializer that is not a "
                                 "literal or a call at line ",
                                 (long long)vd->line.line));
            }
            EmitDirectCall(rc, w, nullptr, &r);
            return;
        }
        if (auto *rib = init ? Is<InlineBlock>(init) : nullptr) {
            if (!rib->spec || rib->spec->rets.size() != 1
                || !is_growable_i64(rib->spec->rets[0])) {
                w.rzarrs.erase(&def);
                return Unsup(cat("a growable initializer that is not a "
                                 "literal or an inlined call at line ",
                                 (long long)vd->line.line));
            }
            GenInline(rib, w, &r);
            return;
        }
        if (strinit) {
            auto *n = llvm::ConstantInt::get(i64(), (uint64_t)sl->val.size());
            if (!sl->val.empty())
                b.CreateMemCpy(basev, llvm::MaybeAlign(1), strlit_glob(sl->val),
                               llvm::MaybeAlign(1), (uint64_t)sl->val.size());
            b.CreateStore(n, lena);
            rz_sync_slot(r, basev, n);
            return;
        }
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
    // An integer-valued expression the walk leaves without a value is a
    // construct it does not emit yet.
    llvm::Value *GenExpr(Node *n, FnWalk &w) {
        auto *v = GenExprRaw(n, w);
        // A push is a statement whose walk yields no value.
        auto *pc = Is<Call>(n);
        auto valueless = pc && pc->builtin == B_PUSH;
        // A block terminated by a return contributes no value: the walk
        // continues only for the statement that follows (dead code).
        if (!v && n && n->exprtype && n->exprtype->kind != TY_VOID && !valueless
            && !b.GetInsertBlock()->getTerminator())
            Unsup(cat("an expression of kind ", (int)n->nkind, " at line ", (long long)n->line.line));
        return v;
    }
    llvm::Value *GenExprRaw(Node *n, FnWalk &w) {
        if (!n) return nullptr;

        // A block in a value position: the base-case inliner makes one of a
        // self-call (bindings, then an if/else choosing the arms), the way
        // the C backend's GenX reads a Block. Its statements run here and the
        // tail is the value; a tail that returns has none.
        if (auto *blk = Is<Block>(n)) {
            for (auto *st : blk->stmts) GenStmt(st, w);
            if (!blk->tail) return nullptr;
            if (auto *tr = Is<Return>(blk->tail)) {
                GenStmt(tr, w);
                return nullptr;
            }
            return GenExpr(blk->tail, w);
        }
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
                case T_BITNOT:
                    return b.CreateXor(c, llvm::ConstantInt::get(i64(), (uint64_t)-1),
                                       "bitnot");
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
            if (bin->op == T_ANDAND || bin->op == T_OROR) {
                // Short-circuit: the right operand runs only where the left
                // does not decide, and the result is an i1 merged by a phi.
                auto isand = bin->op == T_ANDAND;
                auto *lv = GenExpr(bin->left, w);
                if (!lv) return nullptr;
                auto *lhsb = b.GetInsertBlock();
                auto *rhsbb = llvm::BasicBlock::Create(ctx, "logrhs", w.fn);
                auto *donebb = llvm::BasicBlock::Create(ctx, "logdone", w.fn);
                if (isand) b.CreateCondBr(lv, rhsbb, donebb);
                else b.CreateCondBr(lv, donebb, rhsbb);
                b.SetInsertPoint(rhsbb);
                auto *rv = GenExpr(bin->right, w);
                if (!rv) return nullptr;
                auto *rend = b.GetInsertBlock();
                b.CreateBr(donebb);
                b.SetInsertPoint(donebb);
                auto *phi = b.CreatePHI(llvm::Type::getInt1Ty(ctx), 2, "logic");
                phi->addIncoming(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), isand ? 0 : 1), lhsb);
                phi->addIncoming(rv, rend);
                return phi;
            }
            auto *l = GenExpr(bin->left, w);
            auto *r = GenExpr(bin->right, w);
            if (!l || !r) return nullptr;
            switch (bin->op) {
                case T_PLUS:  return b.CreateAdd(l, r, "add");
                case T_MINUS: return b.CreateSub(l, r, "sub");
                case T_MUL:   return b.CreateMul(l, r, "mul");
                case T_DIV:   return GenDivMod(T_DIV, l, r, w, is_uint_ty(bin->exprtype));
                case T_MOD:   return GenDivMod(T_MOD, l, r, w, is_uint_ty(bin->exprtype));
                case T_SHR: {
                    auto *sh = b.CreateAnd(r, llvm::ConstantInt::get(i64(), 63), "shn");
                    return is_uint_ty(bin->left->exprtype) ? b.CreateLShr(l, sh, "shr")
                                                           : b.CreateAShr(l, sh, "shr");
                }
                case T_SHL: {
                    auto *sh = b.CreateAnd(r, llvm::ConstantInt::get(i64(), 63), "shn");
                    return b.CreateShl(l, sh, "shl");
                }
                case T_BITAND: return b.CreateAnd(l, r, "bitand");
                case T_BITOR:  return b.CreateOr(l, r, "bitor");
                case T_XOR:    return b.CreateXor(l, r, "xor");
                case T_EQ:
                case T_NEQ: {
                    auto *lt = bin->left->exprtype;
                    if (lt && is_flat_struct(lt))
                        return struct_eq(l, r, lt->struc->inst,
                                         bin->op == T_NEQ);
                    return bin->op == T_EQ ? b.CreateICmpEQ(l, r, "eq")
                                           : b.CreateICmpNE(l, r, "ne");
                }
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
                // A reference reads out the pointee's value: the pointer
                // it names, then the int stored there.
                auto rit = w.refs.find(id->vdef);
                if (rit != w.refs.end()) {
                    auto *p = b.CreateLoad(i8ptr(), rit->second.base, "refp");
                    if (is_flat_struct(rit->second.ptee))
                        return b.CreateLoad(
                            struct_ll_ty(rit->second.ptee->struc->inst), p,
                            llvm::Twine(id->vdef->name));
                    auto *et = int_ll_ty(rit->second.ptee);
                    auto *v = b.CreateLoad(et, p, llvm::Twine(id->vdef->name));
                    if (et == i64()) return v;
                    return is_uint_ty(rit->second.ptee)
                        ? b.CreateZExt(v, i64()) : b.CreateSExt(v, i64());
                }
                // A flat int struct local as a value.
                auto stit = w.structs.find(id->vdef);
                if (stit != w.structs.end()) {
                    auto *inst = stit->second.inst;
                    return b.CreateLoad(struct_ll_ty(inst), stit->second.base,
                                        "stv");
                }
                // A slice local as a value: the { data, len } it holds.
                auto sit = w.slices.find(id->vdef);
                if (sit != w.slices.end()) {
                    auto *hdr = b.CreateLoad(slice_ll_ty(), sit->second.base, "sv");
                    return slice_struct(b.CreateExtractValue(hdr, 0, "svd"),
                                        b.CreateExtractValue(hdr, 1, "svn"));
                }
            }
            return nullptr;
        }
        if (auto *dt = Is<Dot>(n)) {
            // A field of a flat struct: a field path from a local base, the
            // leaf read at its own width.
            if (dt->IsField()) {
                if (auto *v = GenFieldRead(dt, w)) return v;
            }
            // A member expression: this slice lowers `.len` and `.cap`
            // (member is a BuiltinKind, ast.h) on an array local the walk
            // holds.
            if (dt->member == B_LEN || dt->member == B_CAP) {
                // A member on a local: limited cap/len, fixed len, growable
                // len, or a slice's len via slice_value.
                auto *obj = Is<Ident>(dt->obj);
                if (obj && obj->vdef) {
                    if (auto lit = w.limiteds.find(obj->vdef);
                        lit != w.limiteds.end()) {
                        if (dt->member == B_CAP)
                            return llvm::ConstantInt::get(
                                i64(), (uint64_t)lit->second.cap);
                        return b.CreateLoad(
                            i64(), lit->second.len,
                            llvm::Twine(obj->vdef->name, ".lenv"));
                    }
                    if (auto fit = w.fixeds.find(obj->vdef);
                        fit != w.fixeds.end() && dt->member == B_LEN)
                        return llvm::ConstantInt::get(i64(),
                                                      (uint64_t)fit->second.count);
                    if (dt->member == B_LEN) {
                        if (auto *r = rz_head(w, obj->vdef))
                            return b.CreateLoad(i64(), r->len,
                                                llvm::Twine(obj->vdef->name, ".lenv"));
                    }
                }
                // A slice value (local, range, strlit, block, inline, call):
                // its .len is the view's length.
                if (dt->member == B_LEN) {
                    SliceVal sv;
                    if (slice_value(dt->obj, w, sv)) return sv.len;
                }
                // A call or inlined body returning a growable: the result
                // lives on a stack whose header holds the count. .cap is
                // not meaningful for a growable result (the runtime region
                // has no declared capacity), so only .len lowers here.
                if (dt->member == B_LEN) {
                    if (auto *c = Is<Call>(dt->obj)) {
                        if (c->spec && c->spec->rets.size() == 1
                            && is_growable_i64(c->spec->rets[0])) {
                            auto *v = EmitDirectCall(c, w, nullptr);
                            if (!v) return nullptr;
                            // The call returns void; the growable was built
                            // on a destination stack. For now this path is
                            // not taken: growable results use out-pointers.
                            // A single-result growable returned by value
                            // would need the M3 destination-stack ABI.
                            return nullptr;
                        }
                    }
                }
                // A struct field that is itself an array or slice: walk the
                // field path to the leaf, then read its .len or .cap.
                if (auto *fdt = Is<Dot>(dt->obj); fdt && fdt->IsField()) {
                    llvm::Value *addr = nullptr;
                    StructInst *oi = nullptr;
                    TypeExpr *leaf = nullptr;
                    if (field_path_addr(dt->obj, w, addr, oi, leaf)) {
                        if (is_growable_i64(leaf) && dt->member == B_LEN) {
                            // A growable field: its header is at the field's
                            // address (a gs_rhdr { base, len }).
                            auto *lenp = b.CreateInBoundsGEP(
                                slice_ll_ty(), addr, { idx0(), fld(1) }, "fldlen");
                            return b.CreateLoad(i64(), lenp, "fldlenv");
                        }
                        if (leaf->kind == TY_SLICE && is_int_ty(leaf->sub)
                            && dt->member == B_LEN) {
                            auto *lenp = b.CreateInBoundsGEP(
                                slice_ll_ty(), addr, { idx0(), fld(1) }, "fldlen");
                            return b.CreateLoad(i64(), lenp, "fldlenv");
                        }
                        if (leaf->kind == TY_ARRAY
                            && leaf->arr->akind == A_FIXED
                            && is_int_ty(leaf->arr->sub)
                            && dt->member == B_LEN) {
                            return llvm::ConstantInt::get(
                                i64(), (uint64_t)leaf->arr->size);
                        }
                        if (leaf->kind == TY_ARRAY
                            && leaf->arr->akind == A_LIMITED
                            && is_int_ty(leaf->arr->sub)) {
                            if (dt->member == B_CAP)
                                return llvm::ConstantInt::get(
                                    i64(), (uint64_t)leaf->arr->size);
                            if (dt->member == B_LEN) {
                                // Limited field: len is stored after the
                                // element slots. The layout is
                                // [cap x elem][i64 len].
                                auto *et = int_ll_ty(leaf->arr->sub);
                                auto *lenp = b.CreateInBoundsGEP(
                                    et, addr,
                                    { llvm::ConstantInt::get(i64(),
                                                             (uint64_t)leaf->arr->size) },
                                    "fldlimlen");
                                return b.CreateLoad(i64(), lenp, "fldlimlenv");
                            }
                        }
                    }
                }
            }
            return nullptr;
        }
        if (auto *ix = Is<Index>(n)) {
            // An element read a[i]: bounds check (fixed arrays check
            // against their constant count), then load base[i]. The
            // comparison is unsigned, so a negative i64 index is caught
            // by the same check.
            auto *obj = Is<Ident>(ix->obj);
            if (obj && obj->vdef) {
                if (auto fit = w.fixeds.find(obj->vdef); fit != w.fixeds.end()) {
                    auto *idxv = GenExpr(ix->idx, w);
                    if (!idxv) return nullptr;
                    if (!ix->nobc) {
                        auto *okbb = llvm::BasicBlock::Create(ctx, "fxok", w.fn);
                        auto *failbb = llvm::BasicBlock::Create(ctx, "fxfail", w.fn);
                        b.CreateCondBr(b.CreateICmpULT(idxv,
                            llvm::ConstantInt::get(i64(), (uint64_t)fit->second.count),
                            "fxok"), okbb, failbb);
                        b.SetInsertPoint(failbb);
                        std::vector<llvm::Value *> faila = {
                            idxv,
                            llvm::ConstantInt::get(i64(), fit->second.count),
                            llvm::ConstantPointerNull::get(i8ptr()),
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(ctx), 0) };
                        b.CreateCall(f_gs_idxfail, faila);
                        b.CreateUnreachable();
                        b.SetInsertPoint(okbb);
                    }
                    auto *et = int_ll_ty(fit->second.etype);
                    auto *base = fit->second.base;
                    auto *p = fit->second.count
                        ? b.CreateInBoundsGEP(base->getAllocatedType(), base,
                            { llvm::ConstantInt::get(i64(), 0), idxv }, "fxip")
                        : nullptr;
                    auto *v = b.CreateLoad(et, p, "fxval");
                    if (et == i64()) return v;
                    return is_uint_ty(fit->second.etype)
                        ? b.CreateZExt(v, i64()) : b.CreateSExt(v, i64());
                }
                if (auto lit = w.limiteds.find(obj->vdef);
                    lit != w.limiteds.end()) {
                    auto &lm = lit->second;
                    auto *idxv = GenExpr(ix->idx, w);
                    if (!idxv) return nullptr;
                    auto *lenv = b.CreateLoad(i64(), lm.len, "limlen");
                    if (!ix->nobc) {
                        auto *okbb = llvm::BasicBlock::Create(ctx, "limok", w.fn);
                        auto *failbb = llvm::BasicBlock::Create(ctx, "limfail", w.fn);
                        b.CreateCondBr(b.CreateICmpULT(idxv, lenv, "limin"),
                                       okbb, failbb);
                        b.SetInsertPoint(failbb);
                        b.CreateCall(f_gs_idxfail, {
                            idxv, lenv,
                            llvm::ConstantPointerNull::get(i8ptr()),
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(ctx), 0) });
                        b.CreateUnreachable();
                        b.SetInsertPoint(okbb);
                    }
                    auto *et = int_ll_ty(lm.etype);
                    auto *p = b.CreateInBoundsGEP(lm.base->getAllocatedType(),
                        lm.base, { llvm::ConstantInt::get(i64(), 0), idxv },
                        "limip");
                    auto *v = b.CreateLoad(et, p, "limval");
                    if (et == i64()) return v;
                    return is_uint_ty(lm.etype) ? b.CreateZExt(v, i64())
                                                : b.CreateSExt(v, i64());
                }
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
            // A view: a slice local, a range, or a range of one. The subject
            // is read out even when it is not a plain variable (a[1..4][0]).
            SliceVal sv;
            if (slice_value(ix->obj, w, sv)) {
                auto *idxv = GenExpr(ix->idx, w);
                if (!idxv) return nullptr;
                if (!ix->nobc) {
                    auto *okbb = llvm::BasicBlock::Create(ctx, "slixok", w.fn);
                    auto *failbb = llvm::BasicBlock::Create(ctx, "slixfail", w.fn);
                    b.CreateCondBr(b.CreateICmpULT(idxv, sv.len, "slixin"),
                                   okbb, failbb);
                    b.SetInsertPoint(failbb);
                    b.CreateCall(f_gs_idxfail, {
                        idxv, sv.len,
                        llvm::ConstantPointerNull::get(i8ptr()),
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(ctx), 0) });
                    b.CreateUnreachable();
                    b.SetInsertPoint(okbb);
                }
                auto *et = int_ll_ty(sv.etype);
                auto *v = b.CreateLoad(
                    et, b.CreateInBoundsGEP(et, sv.data, idxv, "slip"), "slval");
                if (et == i64()) return v;
                return is_uint_ty(sv.etype) ? b.CreateZExt(v, i64())
                                            : b.CreateSExt(v, i64());
            }
            return nullptr;
        }
        if (auto *c = Is<Call>(n)) {
            // push (memberof a.f spelling: the receiver is the callee's
            // Dot obj, args holds only the value) on a growable i64 array
            // local: store the element at base+len, bump len, and keep the
            // runtime slot's top at the next free byte (the contract of
            // the runtime's section 1.2).
            // assert(x) is the runtime abort on a false test (GS_E_ASSERT),
            // exactly as the C backend's gs_abort check.
            if (c->builtin == B_ASSERT && c->args.size() == 1) {
                auto *v = GenExpr(c->args[0], w);
                if (!v) return nullptr;
                auto *bad = v->getType()->isIntegerTy(1)
                    ? b.CreateNot(v, "assertnot")
                    : b.CreateICmpEQ(v, llvm::ConstantInt::get(i64(), 0),
                                     "assertnot");
                auto *failbb = llvm::BasicBlock::Create(ctx, "assertfail", w.fn);
                auto *okbb = llvm::BasicBlock::Create(ctx, "assertok", w.fn);
                b.CreateCondBr(bad, failbb, okbb);
                b.SetInsertPoint(failbb);
                b.CreateCall(f_gs_abort, {
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 3),
                    llvm::ConstantPointerNull::get(i8ptr()),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
                                           (uint64_t)c->line.line) });
                b.CreateUnreachable();
                b.SetInsertPoint(okbb);
                return nullptr;
            }
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
                    // A static-capacity limited receiver: the same shape,
                    // with the element count's growth bounded by the
                    // declared capacity (GS_E_CAPACITY past it).
                    if (auto lit = w.limiteds.find(recv->vdef);
                        lit != w.limiteds.end()) {
                        auto *v = GenExpr(c->args[0], w);
                        if (!v) return nullptr;
                        auto &lm = lit->second;
                        auto *nl = b.CreateLoad(i64(), lm.len, "limlen");
                        auto *okbb = llvm::BasicBlock::Create(ctx, "pushok", w.fn);
                        auto *failbb = llvm::BasicBlock::Create(ctx, "pushcap", w.fn);
                        b.CreateCondBr(b.CreateICmpULT(
                            nl, llvm::ConstantInt::get(
                                i64(), (uint64_t)lm.cap), "pushin"),
                            okbb, failbb);
                        b.SetInsertPoint(failbb);
                        b.CreateCall(f_gs_abort, {
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(ctx), 0),
                            llvm::ConstantPointerNull::get(i8ptr()),
                            llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(ctx),
                                (uint64_t)c->line.line) });
                        b.CreateUnreachable();
                        b.SetInsertPoint(okbb);
                        auto *et = int_ll_ty(lm.etype);
                        auto *p = b.CreateInBoundsGEP(lm.base->getAllocatedType(),
                            lm.base, { llvm::ConstantInt::get(i64(), 0), nl },
                            "puship");
                        b.CreateStore(et == i64() ? v : b.CreateTrunc(v, et), p);
                        b.CreateStore(b.CreateAdd(
                            nl, llvm::ConstantInt::get(i64(), 1), "limlen+1"),
                            lm.len);
                        return nullptr;
                    }
                }
            }
            // append (memberof spelling: the receiver is the callee's Dot
            // obj) onto a growable int array the walk holds. The source's
            // elements are evaluated first, as the C backend evaluates
            // them, copied to the top, and the length grows by their count.
            if (c->builtin == B_APPEND && c->args.size() == 1) {
                auto *rd = Is<Dot>(c->callee);
                auto *recv = rd ? Is<Ident>(rd->obj) : nullptr;
                auto *r = recv && recv->vdef ? rz_head(w, recv->vdef) : nullptr;
                auto lit = recv && recv->vdef
                    ? w.limiteds.find(recv->vdef) : w.limiteds.end();
                if (!r && lit == w.limiteds.end())
                    return Unsup(cat("an append onto a variable the walk does "
                                     "not hold at line ",
                                     (long long)c->line.line)), nullptr;
                bool byteelem = (r ? r->etype : lit->second.etype)->intstorage
                                    == IS_U8
                    || (r ? r->etype : lit->second.etype)->intstorage == IS_I8;
                llvm::Value *data = nullptr, *n = nullptr;
                if (auto *sl = Is<StrLit>(c->args[0])) {
                    if (!byteelem)
                        return Unsup(cat("an append of a string literal onto a "
                                         "non-byte array at line ",
                                         (long long)c->line.line)), nullptr;
                    data = strlit_glob(sl->val);
                    n = llvm::ConstantInt::get(i64(), (uint64_t)sl->val.size());
                } else {
                    auto *st = c->args[0]->exprtype;
                    SliceVal sv;
                    if ((!st || (st->kind != TY_SLICE && st->kind != TY_ARRAY))
                        || !slice_value(c->args[0], w, sv)) {
                        // An if arm or block tail the value path built into
                        // the { data, len } pair.
                        auto *v = st && is_slice_of_int(st)
                            ? GenExpr(c->args[0], w) : nullptr;
                        if (!v || v->getType() != slice_ll_ty())
                            return Unsup(cat("an append source the walk does "
                                             "not build at line ",
                                             (long long)c->line.line)), nullptr;
                        sv.data = b.CreateExtractValue(v, 0, "apd");
                        sv.len = b.CreateExtractValue(v, 1, "apn");
                    }
                    data = sv.data;
                    n = sv.len;
                }
                auto *et = int_ll_ty(r ? r->etype : lit->second.etype);
                auto *esz = llvm::ConstantInt::get(
                    i64(), (uint64_t)(et->getScalarSizeInBits() / 8));
                if (r) {
                    auto *basev = b.CreateLoad(i8ptr(), r->base, "rzbase");
                    auto *lenv = b.CreateLoad(i64(), r->len, "rzlen");
                    auto *dst = b.CreateInBoundsGEP(et, basev, lenv, "apdst");
                    b.CreateMemCpy(dst, llvm::MaybeAlign(1), data,
                                   llvm::MaybeAlign(1),
                                   b.CreateMul(n, esz, "apbytes"));
                    auto *nlen = b.CreateAdd(lenv, n, "rzlen+");
                    b.CreateStore(nlen, r->len);
                    rz_sync_slot(*r, basev, nlen);
                } else {
                    auto &lm = lit->second;
                    auto *ol = b.CreateLoad(i64(), lm.len, "limlen");
                    // The declared capacity bounds the run: past it the
                    // runtime's GS_E_CAPACITY abort, as the C backend
                    // emits it (codegen_builtins.h).
                    auto *okbb = llvm::BasicBlock::Create(ctx, "apok", w.fn);
                    auto *failbb = llvm::BasicBlock::Create(ctx, "apcap", w.fn);
                    b.CreateCondBr(b.CreateICmpULE(
                        b.CreateAdd(ol, n, "limend"),
                        llvm::ConstantInt::get(i64(), (uint64_t)lm.cap),
                        "apin"), okbb, failbb);
                    b.SetInsertPoint(failbb);
                    b.CreateCall(f_gs_abort, {
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(ctx), 0),
                        llvm::ConstantPointerNull::get(i8ptr()),
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(ctx),
                            (uint64_t)c->line.line) });
                    b.CreateUnreachable();
                    b.SetInsertPoint(okbb);
                    auto *dst = b.CreateInBoundsGEP(
                        lm.base->getAllocatedType(), lm.base,
                        { llvm::ConstantInt::get(i64(), 0), ol }, "apdst");
                    b.CreateMemCpy(dst, llvm::MaybeAlign(1), data,
                                   llvm::MaybeAlign(1),
                                   b.CreateMul(n, esz, "apbytes"));
                    b.CreateStore(b.CreateAdd(ol, n, "limlen+"), lm.len);
                }
                return nullptr;
            }
            // pop (memberof spelling) from a growable int array the walk
            // holds: on empty it is the runtime's GS_E_POP abort, else the
            // length drops by one and the element's value is read back from
            // the slot it leaves (whose bytes nothing overwrites until the
            // array grows again).
            if (c->builtin == B_POP && c->args.empty()) {
                auto *rd = Is<Dot>(c->callee);
                auto *recv = rd ? Is<Ident>(rd->obj) : nullptr;
                auto *r = recv && recv->vdef ? rz_head(w, recv->vdef) : nullptr;
                auto lit = recv && recv->vdef
                    ? w.limiteds.find(recv->vdef) : w.limiteds.end();
                if (!r && lit == w.limiteds.end())
                    return Unsup(cat("a pop from a variable the walk does not "
                                     "hold at line ", (long long)c->line.line)),
                        nullptr;
                auto *basev = r ? b.CreateLoad(i8ptr(), r->base, "rzbase") : nullptr;
                auto *lenv = r ? b.CreateLoad(i64(), r->len, "rzlen")
                    : b.CreateLoad(i64(), lit->second.len, "limlen");
                if (!c->nobc) {
                    auto *okbb = llvm::BasicBlock::Create(ctx, "popok", w.fn);
                    auto *failbb = llvm::BasicBlock::Create(ctx, "popfail", w.fn);
                    b.CreateCondBr(
                        b.CreateICmpSGT(lenv, llvm::ConstantInt::get(i64(), 0),
                                        "popin"), okbb, failbb);
                    b.SetInsertPoint(failbb);
                    b.CreateCall(f_gs_abort, {
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 7),
                        llvm::ConstantPointerNull::get(i8ptr()),
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
                                               (uint64_t)c->line.line) });
                    b.CreateUnreachable();
                    b.SetInsertPoint(okbb);
                }
                auto *nlen = b.CreateSub(lenv, llvm::ConstantInt::get(i64(), 1),
                                         "len-1");
                if (r) {
                    b.CreateStore(nlen, r->len);
                    rz_sync_slot(*r, basev, nlen);
                    return rz_load_elem(*r, basev, nlen, "rzpop");
                }
                b.CreateStore(nlen, lit->second.len);
                auto *et = int_ll_ty(lit->second.etype);
                auto *ev = b.CreateLoad(
                    et, b.CreateInBoundsGEP(et, lit->second.base, nlen, "limpop"),
                    "limpop");
                if (et == i64()) return ev;
                return is_uint_ty(lit->second.etype)
                    ? b.CreateZExt(ev, i64()) : b.CreateSExt(ev, i64());
            }
            // pop_n drops the count's last elements at once (the count's
            // argument first, then the length it may itself have changed):
            // one unsigned compare rejects a count past the length and a
            // negative one alike, the runtime's GS_E_POPN abort.
            if (c->builtin == B_POP_N && c->args.size() == 1) {
                auto *rd = Is<Dot>(c->callee);
                auto *recv = rd ? Is<Ident>(rd->obj) : nullptr;
                auto *r = recv && recv->vdef ? rz_head(w, recv->vdef) : nullptr;
                auto lit = recv && recv->vdef
                    ? w.limiteds.find(recv->vdef) : w.limiteds.end();
                if (!r && lit == w.limiteds.end())
                    return Unsup(cat("a pop_n from an array the walk does not "
                                     "hold at line ", (long long)c->line.line)),
                        nullptr;
                auto *nn = GenExpr(c->args[0], w);
                if (!nn) return nullptr;
                auto *lenv = r ? b.CreateLoad(i64(), r->len, "rzlen")
                    : b.CreateLoad(i64(), lit->second.len, "limlen");
                auto *okbb = llvm::BasicBlock::Create(ctx, "pnok", w.fn);
                auto *failbb = llvm::BasicBlock::Create(ctx, "pnfail", w.fn);
                b.CreateCondBr(b.CreateICmpULE(nn, lenv, "pnin"), okbb, failbb);
                b.SetInsertPoint(failbb);
                b.CreateCall(f_gs_abort, {
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 15),
                    llvm::ConstantPointerNull::get(i8ptr()),
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx),
                                           (uint64_t)c->line.line) });
                b.CreateUnreachable();
                b.SetInsertPoint(okbb);
                auto *nlen = b.CreateSub(lenv, nn, "len-n");
                if (r) {
                    b.CreateStore(nlen, r->len);
                    rz_sync_slot(*r, b.CreateLoad(i8ptr(), r->base), nlen);
                } else {
                    b.CreateStore(nlen, lit->second.len);
                }
                return nullptr;
            }
            // print's line alphabet this slice covers: int values and
            // string literals, with no separator of its own, rendered into the same
            // byte cursor the C backend builds its whole line in
            // (codegen_builtins.h B_PRINT), then one write. The cursor
            // runs in a dedicated reserved scratch slot; every print
            // starts it at the base again and resets it after.
            if (c->builtin == B_PRINT) {
                if (!gs_pstk) {
                    gs_pstk = new llvm::GlobalVariable(
                        *mod, i8ptr(), /*isConstant=*/false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(i8ptr()), "gs_pstk",
                        nullptr, llvm::GlobalValue::GeneralDynamicTLSModel);
                    stk_slots.push_back(gs_pstk);
                }
                auto *basev = b.CreateLoad(i8ptr(), gs_pstk, "pline");
                auto *cura = entry_alloca(w, i8ptr(), "pcur");
                b.CreateStore(basev, cura);
                auto *i8ty = llvm::Type::getInt8Ty(ctx);
                for (size_t ai = 0; ai < c->args.size(); ++ai) {
                    auto *an = c->args[ai];
                    if (auto *sl = Is<StrLit>(an)) {
                        emit_print_bytes(cura, sl->val);
                        continue;
                    }
                    if (an->exprtype
                        && (an->exprtype->kind == TY_ARRAY
                            || an->exprtype->kind == TY_SLICE)) {
                        if (!emit_print_view(an, an->exprtype, cura, w))
                            return nullptr;
                        continue;
                    }
                    if (!an->exprtype
                        || (!is_int_ty(an->exprtype)
                            && !is_bool_ty(an->exprtype)))
                        return Unsup("a print of a value that is not an int, a bool or a string literal"), nullptr;
                    auto *x = GenExpr(an, w);
                    if (!x)
                        return Unsup(cat("a print argument at line ",
                                         (long long)an->line.line)), nullptr;
                    auto *cur = b.CreateLoad(i8ptr(), cura);
                    // u64 renders unsigned, as the C backend's hook does;
                    // every narrower int widens to an i64 that fits.
                    auto *hook = is_bool_ty(an->exprtype) ? f_gs_fmt_bool
                        : (an->exprtype->kind == TY_INT
                           && an->exprtype->intstorage == IS_U64)
                            ? f_gs_fmt_u64 : f_gs_fmt_i64;
                    auto *n = b.CreateCall(hook, { cur, as_i64(x) });
                    b.CreateStore(b.CreateInBoundsGEP(i8ty, cur, n, "pcur+"),
                                  cura);
                }
                emit_print_bytes(cura, "\n");
                auto *cur = b.CreateLoad(i8ptr(), cura);
                auto *endv = cur;
                // The length: byte positions of the same region, so the
                // ptrtoint difference is the count. One write, then the
                // cursor resets to the base.
                b.CreateCall(f_gs_out_bytes, { basev,
                    b.CreateSub(
                        b.CreatePtrToInt(endv, i64()),
                        b.CreatePtrToInt(basev, i64()), "pbytes") });
                b.CreateStore(basev, gs_pstk);
                return nullptr;
            }
            // A direct call to a Goose function: all i64 args, i64 ret,
            // and the valueless (void) call whose effect the statement
            // walk keeps.
            // A direct call to a Goose function: all i64 args, ret0 as the
            // call's value, rets 1.. through out-pointer params the
            // multi-return callee writes; a value context needs exactly
            // one result.
            if (!c->dispatch.empty())
                return Unsup(cat("a variant dispatch call at line ",
                                 (long long)c->line.line)), nullptr;
            if (c->spec) {
                auto *v = EmitDirectCall(c, w, nullptr);
                if (!v) return nullptr;
                // A single reference result in a value position reads
                // through it: binding a call's reference to a fixed-size
                // value takes a copy of the pointee (4.1). A void call
                // has no result at all.
                if (c->spec->rets.size() == 1
                    && is_i64_ref(c->spec->rets[0]))
                    return b.CreateLoad(i64(), v, "refv");
                return v;
            }
            // Any other builtin is a construct the walk does not emit: a
            // void one would otherwise vanish from the program.
            if (c->builtin >= 0 && c->builtin != B_PUSH)
                Unsup(cat("a builtin call ", BuiltinByKind(c->builtin).name,
                          " at line ", (long long)c->line.line));
            return nullptr;
        }
        if (auto *ib = Is<InlineBlock>(n)) {
            auto *v = GenInline(ib, w);
            if (!v) return nullptr;
            if (ib->spec && ib->spec->rets.size() == 1
                && is_i64_ref(ib->spec->rets[0]))
                return b.CreateLoad(i64(), v, "refv");
            return v;
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
            // A branch is a block (its statements, then its tail) or one
            // expression. Its value is what the tail yields; a branch that
            // returns yields none and leaves its block terminated. The tail
            // is evaluated exactly once.
            b.SetInsertPoint(thenbb);
            llvm::Value *thenval = ifx->thenb ? GenExpr(ifx->thenb, w) : nullptr;
            auto *thenend = b.GetInsertBlock();
            if (!thenend->getTerminator()) b.CreateBr(merge);
            llvm::Value *elseval = nullptr;
            llvm::BasicBlock *elseend = nullptr;
            if (elsebb) {
                b.SetInsertPoint(elsebb);
                elseval = GenExpr(ifx->elseb, w);
                elseend = b.GetInsertBlock();
                if (!elseend->getTerminator()) b.CreateBr(merge);
            }
            b.SetInsertPoint(merge);
            if (thenval && elseval && thenval != elseval
                && thenval->getType() == elseval->getType()) {
                auto *ty = thenval->getType();
                auto *phi = b.CreatePHI(ty, 2, "ifval");
                phi->addIncoming(thenval, thenend);
                phi->addIncoming(elseval, elseend);
                return phi;
            }
            return thenval ? thenval : elseval;
        }
        // Constructs this walk knows but does not lower yet are refused even
        // when their type is void: dropping them would change the program.
        switch (n->nkind) {
            case NK_MatchExpr: case NK_LoopExpr: case NK_Defer:
            case NK_ConstFor: case NK_ConstIf: case NK_ConstQuery:
                Unsup(cat("a construct of kind ", (int)n->nkind, " at line ",
                          (long long)n->line.line));
                return nullptr;
            default:
                break;
        }
        // A flat int struct where a value is wanted (an argument, an if
        // arm): the value the literal, local or call builds.
        if (n->exprtype && is_flat_struct(n->exprtype)) {
            llvm::Value *sv = nullptr;
            StructInst *si = nullptr;
            if (struct_value(n, w, sv, si)) return sv;
        }
        // A slice where a value is wanted (an if arm, a block's tail): the
        // { data, len } the view names.
        if (n->exprtype && is_slice_of_int(n->exprtype)) {
            SliceVal sv;
            if (slice_value(n, w, sv)) return slice_struct(sv.data, sv.len);
        }
        return nullptr;
    }

    // A check on a division operand: a true condition ends in the runtime's
    // failure (noreturn), so the walk continues in the pass block.
    void div_guard(llvm::Value *bad, llvm::Function *fail, FnWalk &w) {
        auto *failbb = llvm::BasicBlock::Create(ctx, "divfail", w.fn);
        auto *okbb = llvm::BasicBlock::Create(ctx, "divok", w.fn);
        b.CreateCondBr(bad, failbb, okbb);
        b.SetInsertPoint(failbb);
        b.CreateCall(fail, { llvm::ConstantPointerNull::get(i8ptr()),
                             llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0) });
        b.CreateUnreachable();
        b.SetInsertPoint(okbb);
    }

    // A signed `/` or `%` as the runtime's gs_div_i64 / gs_mod_i64: a zero
    // divisor and i64.min / -1 fail; `%` is Euclidean (a negative remainder
    // takes |divisor| on), where `/` truncates. An unsigned operand's
    // values are zero-extended, so its division is the plain udiv/urem (a
    // zero divisor still aborts).
    llvm::Value *GenDivMod(TType op, llvm::Value *l, llvm::Value *r,
                           FnWalk &w, bool uns = false) {
        auto *zero = llvm::ConstantInt::get(i64(), 0);
        auto *minus1 = llvm::ConstantInt::get(i64(), (uint64_t)-1, true);
        auto *intmin = llvm::ConstantInt::get(i64(), (uint64_t)1 << 63);
        div_guard(b.CreateICmpEQ(r, zero, "divzero"), f_gs_divfail, w);
        if (uns)
            return op == T_DIV ? b.CreateUDiv(l, r, "div")
                               : b.CreateURem(l, r, "rem");
        div_guard(b.CreateAnd(b.CreateICmpEQ(l, intmin), b.CreateICmpEQ(r, minus1)),
                  f_gs_divovf, w);
        if (op == T_DIV) return b.CreateSDiv(l, r, "div");
        // i64.min % -1 is 0 (every x % -1): divide by 1 there instead.
        auto *safe = b.CreateSelect(b.CreateICmpEQ(r, minus1), llvm::ConstantInt::get(i64(), 1), r);
        auto *rem = b.CreateSRem(l, safe, "rem");
        auto *absr = b.CreateSelect(b.CreateICmpSLT(r, zero),
                                    b.CreateSub(zero, r), r, "absr");
        auto *adj = b.CreateAdd(rem, absr, "remadj");
        return b.CreateSelect(b.CreateICmpSLT(rem, zero), adj, rem, "mod");
    }

    // Appends source bytes at the print cursor with one store per byte: no
    // global constant and no memcpy calls for text the line itself carries.
    void emit_print_bytes(llvm::Value *cura, string_view bytes) {
        auto *i8ty = llvm::Type::getInt8Ty(ctx);
        auto *cur = b.CreateLoad(i8ptr(), cura);
        for (size_t i = 0; i < bytes.size(); ++i)
            b.CreateStore(llvm::ConstantInt::get(i8ty, (uint8_t)bytes[i]),
                          b.CreateInBoundsGEP(i8ty, cur,
                              llvm::ConstantInt::get(i64(), (uint64_t)i)));
        b.CreateStore(b.CreateInBoundsGEP(i8ty, cur,
            llvm::ConstantInt::get(i64(), (uint64_t)bytes.size())), cura);
    }

    // The slice's bounds check: `lo < 0 || hi < lo || hi > len` aborts as a
    // slice failure (GS_E_SLICE), exactly as the C backend's GenSlice does.
    void slice_check(llvm::Value *lo, llvm::Value *hi, llvm::Value *lenv,
                     FnWalk &w) {
        auto *zero = llvm::ConstantInt::get(i64(), 0);
        auto *bad = b.CreateOr(b.CreateOr(b.CreateICmpSLT(lo, zero, "slneg"),
                                          b.CreateICmpSLT(hi, lo, "slrev"),
                                          "slrev0"),
                               b.CreateICmpSGT(hi, lenv, "slover"), "slbad");
        auto *okbb = llvm::BasicBlock::Create(ctx, "slok", w.fn);
        auto *badbb = llvm::BasicBlock::Create(ctx, "slabort", w.fn);
        b.CreateCondBr(bad, badbb, okbb);
        b.SetInsertPoint(badbb);
        b.CreateCall(f_gs_abort,
                     { llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1),
                       llvm::ConstantPointerNull::get(i8ptr()),
                       llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0) });
        b.CreateUnreachable();
        b.SetInsertPoint(okbb);
    }

    // The view an array-family expression names: a slice local, a range over
    // one of the array locals the walk holds, or a range over another such
    // view. False when the walk does not hold the subject.
    bool slice_value(Node *n, FnWalk &w, SliceVal &out) {
        if (auto *bk = Is<Block>(n)) {
            if (!n->exprtype || !is_slice_of_int(n->exprtype)) return false;
            auto *v = GenExpr(n, w);
            if (!v || v->getType() != slice_ll_ty()) return false;
            out.data = b.CreateExtractValue(v, 0, "svd");
            out.len = b.CreateExtractValue(v, 1, "svn");
            out.etype = n->exprtype->sub;
            return true;
        }
        // An inlined call whose result is a slice: the body's PHI is the
        // { data, len } it built. A growable result is built into a scratch
        // run destination and read back.
        if (auto *ib = Is<InlineBlock>(n)) {
            if (!ib->spec || ib->spec->rets.size() != 1
                || !is_slice_of_int(ib->spec->rets[0])) {
                if (ib->spec && ib->spec->rets.size() == 1
                    && is_growable_i64(ib->spec->rets[0])) {
                    auto *slot = new llvm::GlobalVariable(
                        *mod, i8ptr(), /*isConstant=*/false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(i8ptr()),
                        cat("gs_stk", stk_slots.size()), nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    stk_slots.push_back(slot);
                    auto *hdra = entry_alloca(w, slice_ll_ty(), "gscr.hdr");
                    auto *basea = b.CreateInBoundsGEP(
                        slice_ll_ty(), hdra, { idx0(), fld(0) }, "gscr.base");
                    auto *lena = b.CreateInBoundsGEP(
                        slice_ll_ty(), hdra, { idx0(), fld(1) }, "gscr.len");
                    auto *basev = b.CreateLoad(i8ptr(), slot, "gscr.top");
                    b.CreateStore(basev, basea);
                    b.CreateStore(llvm::ConstantInt::get(i64(), 0), lena);
                    FnWalk::RzArr scratch { basea, lena, slot, hdra,
                                            ib->spec->rets[0]->arr->sub };
                    GenInline(ib, w, &scratch);
                    out.data = b.CreateLoad(i8ptr(), basea, "svd");
                    out.len = b.CreateLoad(i64(), lena, "svn");
                    out.etype = ib->spec->rets[0]->arr->sub;
                    return true;
                }
                return false;
            }
            auto *v = GenInline(ib, w);
            if (!v || v->getType() != slice_ll_ty()) return false;
            out.data = b.CreateExtractValue(v, 0, "svd");
            out.len = b.CreateExtractValue(v, 1, "svn");
            out.etype = ib->spec->rets[0]->sub;
            return true;
        }
        // A call whose one result is a slice: the { data, len } it returns;
        // a growable result is built into a scratch run destination and
        // read back as its { data, len }.
        if (auto *c = Is<Call>(n)) {
            if (!c->spec || c->spec->rets.size() != 1
                || !is_slice_of_int(c->spec->rets[0])) {
                if (c->spec && c->spec->rets.size() == 1
                    && is_growable_i64(c->spec->rets[0])) {
                    // The run form: a scratch slot of the walk's own, the
                    // call writing its run into it (see emit_growable_into).
                    auto *slot = new llvm::GlobalVariable(
                        *mod, i8ptr(), /*isConstant=*/false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantPointerNull::get(i8ptr()),
                        cat("gs_stk", stk_slots.size()), nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    stk_slots.push_back(slot);
                    auto *hdra = entry_alloca(w, slice_ll_ty(), "gscr.hdr");
                    auto *basea = b.CreateInBoundsGEP(
                        slice_ll_ty(), hdra, { idx0(), fld(0) }, "gscr.base");
                    auto *lena = b.CreateInBoundsGEP(
                        slice_ll_ty(), hdra, { idx0(), fld(1) }, "gscr.len");
                    auto *basev = b.CreateLoad(i8ptr(), slot, "gscr.top");
                    b.CreateStore(basev, basea);
                    b.CreateStore(llvm::ConstantInt::get(i64(), 0), lena);
                    FnWalk::RzArr scratch { basea, lena, slot, hdra,
                                            c->spec->rets[0]->arr->sub };
                    if (!EmitDirectCall(c, w, nullptr, &scratch)) return false;
                    out.data = b.CreateLoad(i8ptr(), basea, "svd");
                    out.len = b.CreateLoad(i64(), lena, "svn");
                    out.etype = c->spec->rets[0]->arr->sub;
                    return true;
                }
                return false;
            }
            auto *v = EmitDirectCall(c, w, nullptr);
            if (!v) return false;
            out.data = b.CreateExtractValue(v, 0, "svd");
            out.len = b.CreateExtractValue(v, 1, "svn");
            out.etype = c->spec->rets[0]->sub;
            return true;
        }
        if (auto *al = Is<ArrayLit>(n)) {
            // A literal printed or appended whole (a call folded to one):
            // its elements land in a temporary array, the view is that
            // array's { base, count }.
            auto *et = n->exprtype;
            if (!et || et->kind != TY_ARRAY || !is_int_ty(et->arr->sub))
                return false;
            if (al->fillval) {
                auto *cnt = al->fillcount ? GenExpr(al->fillcount, w) : nullptr;
                if (!cnt || !llvm::isa<llvm::ConstantInt>(cnt)) return false;
                auto *arr = entry_alloca(
                    w, llvm::ArrayType::get(int_ll_ty(et->arr->sub),
                                            (uint64_t)llvm::cast<llvm::ConstantInt>(cnt)->getZExtValue()),
                    "alit");
                auto *fv = GenExpr(al->fillval, w);
                if (!fv) return false;
                auto *etl = int_ll_ty(et->arr->sub);
                auto cnt64 = llvm::cast<llvm::ConstantInt>(cnt)->getZExtValue();
                for (uint64_t k = 0; k < cnt64; k++)
                    b.CreateStore(etl == i64() ? fv : b.CreateTrunc(fv, etl),
                                  b.CreateInBoundsGEP(
                                      arr->getAllocatedType(), arr,
                                      { llvm::ConstantInt::get(i64(), 0),
                                        llvm::ConstantInt::get(i64(), k) },
                                      "alitp"));
                out.data = b.CreateInBoundsGEP(
                    arr->getAllocatedType(), arr,
                    { llvm::ConstantInt::get(i64(), 0),
                      llvm::ConstantInt::get(i64(), 0) }, "alitd");
                out.len = llvm::ConstantInt::get(i64(), cnt64);
                out.etype = et->arr->sub;
                return true;
            }
            if (al->elems.empty()) {
                out.data = llvm::ConstantPointerNull::get(i8ptr());
                out.len = llvm::ConstantInt::get(i64(), 0);
                out.etype = et->arr->sub;
                return true;
            }
            auto *arr = entry_alloca(
                w, llvm::ArrayType::get(int_ll_ty(et->arr->sub),
                                        al->elems.size()),
                "alit");
            auto *etl = int_ll_ty(et->arr->sub);
            for (size_t k = 0; k < al->elems.size(); ++k) {
                auto *v = GenExpr(al->elems[k], w);
                if (!v) return false;
                b.CreateStore(etl == i64() ? v : b.CreateTrunc(v, etl),
                              b.CreateInBoundsGEP(
                                  arr->getAllocatedType(), arr,
                                  { llvm::ConstantInt::get(i64(), 0),
                                    llvm::ConstantInt::get(i64(), (uint64_t)k) },
                                  "alitp"));
            }
            out.data = b.CreateInBoundsGEP(
                arr->getAllocatedType(), arr,
                { llvm::ConstantInt::get(i64(), 0),
                  llvm::ConstantInt::get(i64(), 0) }, "alitd");
            out.len = llvm::ConstantInt::get(i64(), al->elems.size());
            out.etype = et->arr->sub;
            return true;
        }
        if (auto *sl = Is<StrLit>(n)) {
            // A literal's bytes are one internal constant; the view is
            // { bytes, length }, whatever array type the context read it as
            // (a slice, a u8[>..] a call was folded to, a u8[] local's
            // initializer).
            auto *et = n->exprtype;
            if (!et || (et->kind != TY_SLICE && et->kind != TY_ARRAY))
                return false;
            auto *gv = strlit_glob(sl->val);
            out.data = b.CreateInBoundsGEP(
                gv->getValueType(), gv,
                { llvm::ConstantInt::get(i64(), 0), llvm::ConstantInt::get(i64(), 0) },
                "svd");
            out.len = llvm::ConstantInt::get(i64(), (uint64_t)sl->val.size());
            out.etype = et->kind == TY_SLICE ? et->sub : et->arr->sub;
            return true;
        }
        if (auto *obj = Is<Ident>(n)) {
            if (!obj->vdef) return false;
            if (auto sit = w.slices.find(obj->vdef); sit != w.slices.end()) {
                auto &sl = sit->second;
                auto *hdr = b.CreateLoad(slice_ll_ty(), sl.base, "slv");
                out.data = b.CreateExtractValue(hdr, 0, "svd");
                out.len = b.CreateExtractValue(hdr, 1, "svn");
                out.etype = sl.etype;
                return true;
            }
            if (auto fit = w.fixeds.find(obj->vdef); fit != w.fixeds.end()) {
                auto &fx = fit->second;
                // A zero-count array has no alloca: its view is a null
                // pointer and length 0.
                out.data = fx.base
                    ? b.CreateInBoundsGEP(
                        fx.base->getAllocatedType(), fx.base,
                        { llvm::ConstantInt::get(i64(), 0),
                          llvm::ConstantInt::get(i64(), 0) }, "svd")
                    : (llvm::Value *)llvm::ConstantPointerNull::get(i8ptr());
                out.len = llvm::ConstantInt::get(i64(), (uint64_t)fx.count);
                out.etype = fx.etype;
                return true;
            }
            if (auto lit = w.limiteds.find(obj->vdef); lit != w.limiteds.end()) {
                auto &lm = lit->second;
                out.data = b.CreateInBoundsGEP(lm.base->getAllocatedType(),
                    lm.base, { llvm::ConstantInt::get(i64(), 0),
                               llvm::ConstantInt::get(i64(), 0) }, "svd");
                out.len = b.CreateLoad(i64(), lm.len, "svn");
                out.etype = lm.etype;
                return true;
            }
            if (auto *r = rz_head(w, obj->vdef)) {
                out.data = b.CreateLoad(i8ptr(), r->base, "svd");
                out.len = b.CreateLoad(i64(), r->len, "svn");
                out.etype = r->etype;
                return true;
            }
            return false;
        }
        auto *se = Is<SliceExpr>(n);
        if (!se) return false;
        SliceVal base;
        if (!slice_value(se->obj, w, base)) return false;
        if (!is_int_ty(base.etype)) return false;
        auto *lenv = base.len;
        auto bound = [&](Node *bn, bool fromend, llvm::Value *dflt) -> llvm::Value * {
            if (!bn) return dflt;
            auto *x = GenExpr(bn, w);
            if (!x) return nullptr;
            return fromend ? b.CreateSub(lenv, x, "slend") : x;
        };
        auto *lo = bound(se->lo, se->lo_from_end, llvm::ConstantInt::get(i64(), 0));
        auto *hi = bound(se->hi, se->hi_from_end, lenv);
        if (!lo || !hi) return false;
        if (!se->nobc) slice_check(lo, hi, lenv, w);
        out.data = b.CreateInBoundsGEP(int_ll_ty(base.etype), base.data, lo, "svd");
        out.len = b.CreateSub(hi, lo, "svn");
        out.etype = base.etype;
        return true;
    }

    // A reference binding (`let px .= n`, `var r .= n`): one pointer
    // holder whose store is the variable the initializer names. The
    // initializer must be a variable this slice holds for now; an
    // element's or a field's address is a later cut.
    void emit_ref_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        auto *ptee = def.type->ref->sub;
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        // The checker wraps an lvalue initializer in a synthetic `&`
        // (TypeCheck::AutoRef, typecheck_exprs.h:772): the reference names
        // what that `&` covers.
        if (auto *u = init ? Is<Unary>(init) : nullptr)
            if (u->op == T_BITAND)
                init = u->child;
        auto *id = init ? Is<Ident>(init) : nullptr;
        if (is_growable_i64(ptee)) {
            // A reference to a growable array keeps the pair the C backend
            // passes: the header and the stack the array lives on. A push
            // through it lands on the array's top and count.
            llvm::Value *hp = nullptr;
            llvm::Value *sp = nullptr;
            if (!id || !id->vdef || !growable_pair(w, id->vdef, hp, sp))
                return Unsup(cat("a reference bound to a value the walk does "
                                 "not hold at line ",
                                 (long long)vd->line.line));
            auto *holder = entry_alloca(w, rzref_ll_ty(),
                                        llvm::Twine(def.name));
            b.CreateStore(hp, b.CreateStructGEP(rzref_ll_ty(), holder, 0));
            b.CreateStore(sp, b.CreateStructGEP(rzref_ll_ty(), holder, 1));
            w.rzrefs[&def] = { holder, ptee->arr->sub };
            return;
        }
        if (is_flat_struct(ptee)) {
            // A reference to a struct: the address its initializer names is
            // the body every read and write through the holder reaches. A
            // call handing one back yields the pointer itself.
            llvm::Value *ptr = nullptr;
            if (auto *c = init ? Is<Call>(init) : nullptr) {
                if (!c->spec || c->spec->rets.size() != 1
                    || !struct_ref_pointee(c->spec->rets[0]))
                    return Unsup(cat("a reference bound to a value the walk "
                                     "does not hold at line ",
                                     (long long)vd->line.line));
                ptr = EmitDirectCall(c, w, nullptr);
            } else if (auto *ib = init ? Is<InlineBlock>(init) : nullptr) {
                if (!ib->spec || ib->spec->rets.size() != 1
                    || !struct_ref_pointee(ib->spec->rets[0]))
                    return Unsup(cat("a reference bound to a value the walk "
                                     "does not hold at line ",
                                     (long long)vd->line.line));
                ptr = GenInline(ib, w);
            } else {
                ptr = ref_addr(init, w);
            }
            if (!ptr)
                return Unsup(cat("a reference bound to a value the walk does "
                                 "not hold at line ",
                                 (long long)vd->line.line));
            auto *slot = entry_alloca(w, i8ptr(), llvm::Twine(def.name));
            b.CreateStore(ptr, slot);
            w.refs[&def] = { slot, ptee };
            return;
        }
        llvm::Value *ptr = nullptr;
        // The initializer may be a call (or an inlined body) whose single
        // result is the reference itself: the pointer it yields is what the
        // holder keeps.
        if (auto *c = init ? Is<Call>(init) : nullptr) {
            if (!c->spec || c->spec->rets.size() != 1
                || !is_i64_ref(c->spec->rets[0]))
                return Unsup(cat("a reference bound to a value the walk does "
                                 "not hold at line ",
                                 (long long)vd->line.line));
            ptr = EmitDirectCall(c, w, nullptr);
        } else if (auto *ib = init ? Is<InlineBlock>(init) : nullptr) {
            if (!ib->spec || ib->spec->rets.size() != 1
                || !is_i64_ref(ib->spec->rets[0]))
                return Unsup(cat("a reference bound to a value the walk does "
                                 "not hold at line ",
                                 (long long)vd->line.line));
            ptr = GenInline(ib, w);
        } else if (id && id->vdef) {
            auto lit = w.locals.find(id->vdef);
            if (lit != w.locals.end()) ptr = lit->second;
            else {
                auto git = global_vars.find(id->vdef);
                if (git != global_vars.end()) ptr = git->second.gv;
            }
        } else if (auto *dt = init ? Is<Dot>(init) : nullptr) {
            // A field of a struct: the address the path walks ends at, with
            // the leaf's own width for every read and write through it.
            llvm::Value *addr = nullptr;
            StructInst *oi = nullptr;
            TypeExpr *leaf = nullptr;
            if (field_path_addr(dt, w, addr, oi, leaf) && is_int_ty(leaf)) {
                ptr = addr;
                ptee = leaf;
            }
        }
        if (!ptr)
            return Unsup(cat("a reference bound to a value the walk does "
                             "not hold at line ", (long long)vd->line.line));
        auto *slot = entry_alloca(w, i8ptr(), llvm::Twine(def.name));
        b.CreateStore(ptr, slot);
        w.refs[&def] = { slot, ptee };
    }

    // The address a reference-valued expression names: what a function
    // returning `i64&` hands back, and what a `.=` binding keeps. A local or
    // global slot, the pointer a reference holds, or a field path's leaf.
    llvm::Value *ref_addr(Node *n, FnWalk &w) {
        if (auto *u = Is<Unary>(n))
            if (u->op == T_BITAND) n = u->child;
        if (auto *id = Is<Ident>(n)) {
            if (!id->vdef) return nullptr;
            auto rit = w.refs.find(id->vdef);
            if (rit != w.refs.end())
                return b.CreateLoad(i8ptr(), rit->second.base, "refp");
            auto lit = w.locals.find(id->vdef);
            if (lit != w.locals.end()) return lit->second;
            auto sit = w.structs.find(id->vdef);
            if (sit != w.structs.end()) return sit->second.base;
            auto git = global_vars.find(id->vdef);
            if (git != global_vars.end()) return git->second.gv;
            return nullptr;
        }
        if (auto *dt = Is<Dot>(n)) {
            llvm::Value *addr = nullptr;
            StructInst *oi = nullptr;
            TypeExpr *leaf = nullptr;
            if (!field_path_addr(dt, w, addr, oi, leaf)) return nullptr;
            if (is_flat_struct(leaf)) return addr;
            if (is_int_ty(leaf) && int_ll_ty(leaf) == i64()) return addr;
            return nullptr;
        }
        return nullptr;
    }

    // `var s: T[:] = <view>`: the view lands in a { data, len } alloca.
    void emit_slice_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        SliceVal sv;
        if (!init || !slice_value(init, w, sv) || !sv.etype)
            return Unsup(cat("a slice value of ", type_label(def.type),
                             " the walk does not build at line ",
                             (long long)vd->line.line));
        auto *slot = entry_alloca(w, slice_ll_ty(), llvm::Twine(def.name));
        auto *hdr = b.CreateInsertValue(llvm::UndefValue::get(slice_ll_ty()),
                                        sv.data, 0);
        hdr = b.CreateInsertValue(hdr, sv.len, 1);
        b.CreateStore(hdr, slot);
        w.slices[&def] = { slot, sv.etype };
    }

    // Appends an int array or slice's text form at the print cursor: raw
    // bytes for u8 elements, `[a, b, c]` otherwise (codegen_render.h).
    bool emit_print_view(Node *an, TypeExpr *t, llvm::Value *cura, FnWalk &w) {
        TypeExpr *etype = nullptr;
        SliceVal sv;
        llvm::Value *lenv = nullptr;
        if (t && t->kind == TY_SLICE) {
            if (!is_slice_of_int(t) || !slice_value(an, w, sv))
                return Unsup(cat("a print of a slice the walk does not hold at line ",
                             (long long)an->line.line)), false;
            etype = sv.etype;
            lenv = sv.len;
        } else if (t && t->kind == TY_ARRAY) {
            // A fixed, length-stored or growable array local or global: the
            // same view slice_value names (a growable one's length is the
            // header's, read here at print time).
            if (!slice_value(an, w, sv))
                return Unsup(cat("a print of an array the walk does not hold at line ",
                             (long long)an->line.line)), false;
            etype = sv.etype;
            lenv = sv.len;
        } else {
            return false;
        }
        auto *i8ty = llvm::Type::getInt8Ty(ctx);
        if (!is_int_ty(etype)) return Unsup("a print of an array element the walk does not render"), false;
        if (etype->intstorage == IS_U8 || etype->intstorage == IS_I8) {
            auto *cur = b.CreateLoad(i8ptr(), cura);
            b.CreateMemMove(cur, llvm::MaybeAlign(1), sv.data, llvm::MaybeAlign(1), lenv);
            b.CreateStore(b.CreateInBoundsGEP(i8ty, cur, lenv, "pcur+"), cura);
            return true;
        }
        auto *zero = llvm::ConstantInt::get(i64(), 0);
        auto *pre = b.GetInsertBlock();
        auto *condbb = llvm::BasicBlock::Create(ctx, "parr_cond", w.fn);
        auto *bodybb = llvm::BasicBlock::Create(ctx, "parr_body", w.fn);
        auto *donebb = llvm::BasicBlock::Create(ctx, "parr_done", w.fn);
        auto *startcur = b.CreateLoad(i8ptr(), cura);
        b.CreateStore(llvm::ConstantInt::get(i8ty, '['), startcur);
        b.CreateStore(b.CreateInBoundsGEP(
            i8ty, startcur, llvm::ConstantInt::get(i64(), 1)), cura);
        b.CreateBr(condbb);
        b.SetInsertPoint(condbb);
        auto *iv = b.CreatePHI(i64(), 2, "parr_i");
        iv->addIncoming(zero, pre);
        b.CreateCondBr(b.CreateICmpSLT(iv, lenv, "parr_test"), bodybb, donebb);
        b.SetInsertPoint(bodybb);
        // A separator before every element but the first.
        auto *sepbb = llvm::BasicBlock::Create(ctx, "parr_sep", w.fn);
        auto *elbb = llvm::BasicBlock::Create(ctx, "parr_el", w.fn);
        b.CreateCondBr(b.CreateICmpEQ(iv, zero, "parr_first"), elbb, sepbb);
        b.SetInsertPoint(sepbb);
        emit_print_bytes(cura, ", ");
        b.CreateBr(elbb);
        b.SetInsertPoint(elbb);
        auto *et = int_ll_ty(etype);
        auto *ev = b.CreateLoad(et, b.CreateInBoundsGEP(et, sv.data, iv, "parr_p"),
                                "parr_v");
        auto *ev64 = et == i64() ? ev
                   : is_uint_ty(etype) ? b.CreateZExt(ev, i64())
                                       : b.CreateSExt(ev, i64());
        auto *cur2 = b.CreateLoad(i8ptr(), cura);
        auto *hook = etype && etype->kind == TY_INT
            && etype->intstorage == IS_U64 ? f_gs_fmt_u64 : f_gs_fmt_i64;
        auto *n = b.CreateCall(hook, { cur2, ev64 });
        b.CreateStore(b.CreateInBoundsGEP(i8ty, cur2, n), cura);
        auto *next = b.CreateAdd(iv, llvm::ConstantInt::get(i64(), 1), "parr_next");
        iv->addIncoming(next, b.GetInsertBlock());
        b.CreateBr(condbb);
        b.SetInsertPoint(donebb);
        auto *endcur = b.CreateLoad(i8ptr(), cura);
        b.CreateStore(llvm::ConstantInt::get(i8ty, ']'), endcur);
        b.CreateStore(b.CreateInBoundsGEP(
            i8ty, endcur, llvm::ConstantInt::get(i64(), 1)), cura);
        return true;
    }

    llvm::StructType *struct_ll_ty(StructInst *in) {
        std::vector<llvm::Type *> elems;
        for (auto *ft : in->ftypes) elems.push_back(field_ll_ty(ft));
        return llvm::StructType::get(ctx, elems);
    }

    llvm::StructType *struct_ty(TypeExpr *t) {
        return struct_ll_ty(t->struc->inst);
    }

    // A flat int struct's value: a local's alloca, a literal built in a
    // temporary, a call whose one result is the struct, or an inlined
    // body's PHI. False when the walk does not hold the value.
    bool struct_value(Node *n, FnWalk &w, llvm::Value *&out, StructInst *&inst) {
        if (auto *bk = Is<Block>(n)) {
            if (!n->exprtype || !is_flat_struct(n->exprtype)) return false;
            auto *v = GenExpr(n, w);
            if (!v) return false;
            out = v;
            inst = n->exprtype->struc->inst;
            return true;
        }
        if (auto *id = Is<Ident>(n)) {
            auto rit = id->vdef ? w.refs.find(id->vdef) : w.refs.end();
            if (rit != w.refs.end() && is_flat_struct(rit->second.ptee)) {
                inst = rit->second.ptee->struc->inst;
                out = b.CreateLoad(
                    struct_ll_ty(inst),
                    b.CreateLoad(i8ptr(), rit->second.base, "refp"), "stv");
                return true;
            }
            auto sit = id->vdef ? w.structs.find(id->vdef) : w.structs.end();
            if (sit == w.structs.end()) return false;
            inst = sit->second.inst;
            out = b.CreateLoad(struct_ll_ty(inst), sit->second.base, "stv");
            return true;
        }
        if (auto *dt = Is<Dot>(n)) {
            // A struct-typed field of a path: the whole value at its place.
            llvm::Value *addr = nullptr;
            StructInst *oi = nullptr;
            TypeExpr *leaf = nullptr;
            if (!field_path_addr(dt, w, addr, oi, leaf) || !is_flat_struct(leaf))
                return false;
            inst = leaf->struc->inst;
            out = b.CreateLoad(struct_ll_ty(inst), addr, "stv");
            return true;
        }
        if (auto *sl = Is<StructLit>(n)) {
            if (!sl->exprtype || !is_flat_struct(sl->exprtype)) return false;
            inst = sl->exprtype->struc->inst;
            if (sl->defaultall || sl->implicit
                || sl->inits.size() != inst->st->fields.size())
                return false;
            std::vector<int> at;
            for (size_t k = 0; k < sl->inits.size(); ++k) {
                int f = field_of_init(inst, sl->inits[k], k);
                if (f < 0 || std::find(at.begin(), at.end(), f) != at.end()
                    || !sl->inits[k].val) return false;
                at.push_back(f);
            }
            auto *slot = entry_alloca(w, struct_ll_ty(inst), "sttmp");
            for (size_t k = 0; k < sl->inits.size(); ++k) {
                auto f = (unsigned)at[k];
                auto *p = b.CreateStructGEP(struct_ll_ty(inst), slot, f);
                if (is_flat_struct(inst->ftypes[f])) {
                    // A nested struct field takes the whole value it built.
                    llvm::Value *fv = nullptr;
                    StructInst *fi = nullptr;
                    if (!struct_value(sl->inits[k].val, w, fv, fi)) return false;
                    b.CreateStore(fv, p);
                    continue;
                }
                auto *v = GenExpr(sl->inits[k].val, w);
                if (!v) return false;
                auto *et = int_ll_ty(inst->ftypes[f]);
                b.CreateStore(et == i64() ? v : b.CreateTrunc(v, et), p);
            }
            out = b.CreateLoad(struct_ll_ty(inst), slot, "stv");
            return true;
        }
        if (auto *c = Is<Call>(n)) {
            if (!c->spec || c->spec->rets.size() != 1
                || !is_flat_struct(c->spec->rets[0])) return false;
            auto *v = EmitDirectCall(c, w, nullptr);
            if (!v) return false;
            out = v;
            inst = c->spec->rets[0]->struc->inst;
            return true;
        }
        if (auto *ib = Is<InlineBlock>(n)) {
            if (!ib->spec || ib->spec->rets.size() != 1
                || !is_flat_struct(ib->spec->rets[0])) return false;
            auto *v = GenInline(ib, w);
            if (!v || v->getType() != struct_ty(ib->spec->rets[0])) return false;
            out = v;
            inst = ib->spec->rets[0]->struc->inst;
            return true;
        }
        return false;
    }

    // Structural equality of two flat int structs: field by field.
    llvm::Value *struct_eq(llvm::Value *l, llvm::Value *r, StructInst *in,
                           bool ne) {
        auto *i1 = llvm::Type::getInt1Ty(ctx);
        llvm::Value *eq = llvm::ConstantInt::getTrue(i1);
        for (size_t f = 0; f < in->ftypes.size(); ++f) {
            auto *a = b.CreateExtractValue(l, (unsigned)f, "sef");
            auto *c = b.CreateExtractValue(r, (unsigned)f, "ser");
            eq = b.CreateAnd(eq, b.CreateICmpEQ(a, c, "seq"), "seqand");
        }
        return ne ? b.CreateNot(eq, "sne") : eq;
    }

    // The field a literal's init names: by name, or by position for a
    // positional init. -1 when the struct has no such field.
    static int field_of_init(StructInst *in, const FieldInit &fi, size_t pos) {
        if (fi.name.empty())
            return pos < in->st->fields.size() ? (int)pos : -1;
        for (size_t i = 0; i < in->st->fields.size(); ++i)
            if (in->st->fields[i].name == fi.name) return (int)i;
        return -1;
    }

    // `var p = T { ... }` for a flat int struct: every field given once, in
    // the literal's order, into the struct's alloca. A literal that leaves a
    // field to a default is refused for now.
    void emit_struct_decl(VarDecl *vd, size_t i, VarDef &def, FnWalk &w) {
        auto *in = def.type->struc->inst;
        auto *init = i < vd->inits.size() ? vd->inits[i] : nullptr;
        auto *sl = init ? Is<StructLit>(init) : nullptr;
        if (!sl) {
            // A copy of another struct value: a local, a call's result or an
            // inlined body's.
            auto *slot = entry_alloca(w, struct_ll_ty(in),
                                      llvm::Twine(def.name));
            llvm::Value *sv = nullptr;
            StructInst *si = nullptr;
            if (!init || !struct_value(init, w, sv, si) || si != in)
                return Unsup(cat("a struct value of type ",
                                 def.type->struc->st->name,
                                 " the walk does not build at line ",
                                 (long long)vd->line.line));
            b.CreateStore(sv, slot);
            w.structs[&def] = { slot, in };
            return;
        }
        if (sl->defaultall || sl->implicit || sl->inits.size() != in->st->fields.size())
            return Unsup(cat("a struct value of type ", def.type->struc->st->name,
                             " the walk does not build at line ", (long long)vd->line.line));
        std::vector<int> at;
        for (size_t k = 0; k < sl->inits.size(); ++k) {
            int f = field_of_init(in, sl->inits[k], k);
            if (f < 0 || std::find(at.begin(), at.end(), f) != at.end() || !sl->inits[k].val)
                return Unsup("a struct literal the walk does not build");
            at.push_back(f);
        }
        auto *slot = entry_alloca(w, struct_ll_ty(in), llvm::Twine(def.name));
        for (size_t k = 0; k < sl->inits.size(); ++k) {
            auto *v = GenExpr(sl->inits[k].val, w);
            if (!v) return;
            auto f = (unsigned)at[k];
            auto *et = int_ll_ty(in->ftypes[f]);
            auto *p = b.CreateStructGEP(slot->getAllocatedType(), slot, f);
            b.CreateStore(et == i64() ? v : b.CreateTrunc(v, et), p);
        }
        w.structs[&def] = { slot, in };
    }

    // `p.f = v` or `p.f op= v` on a flat int struct local.
    // The address of the value a field path names, with the struct it lives
    // in and the type of the leaf: `a`, `a.b`, `a.b.c`. The base is a struct
    // local or the temporary a struct value was built in; every step is a
    // field of the struct before it, and a non-struct field may only end the
    // path. False where the walk does not hold such a place.
    bool field_path_addr(Node *n, FnWalk &w, llvm::Value *&addr,
                         StructInst *&inst, TypeExpr *&leaf) {
        auto *dt = Is<Dot>(n);
        if (!dt || !dt->IsField() || dt->fieldidx < 0) return false;
        // The object side: the base local, or one more field of the path.
        if (auto *odt = Is<Dot>(dt->obj)) {
            llvm::Value *oa = nullptr;
            StructInst *oi = nullptr;
            TypeExpr *ol = nullptr;
            if (!field_path_addr(odt, w, oa, oi, ol)) return false;
            if (!is_flat_struct(ol)) return false;
            inst = ol->struc->inst;
            addr = oa;
        } else {
            auto *obj = Is<Ident>(dt->obj);
            auto sit = obj && obj->vdef ? w.structs.find(obj->vdef)
                                        : w.structs.end();
            auto rit = obj && obj->vdef ? w.refs.find(obj->vdef)
                                        : w.refs.end();
            if (rit != w.refs.end() && is_flat_struct(rit->second.ptee)) {
                // A reference to a struct: the path starts at the pointee
                // the holder names.
                inst = rit->second.ptee->struc->inst;
                addr = b.CreateLoad(i8ptr(), rit->second.base, "refp");
            } else if (sit != w.structs.end()) {
                inst = sit->second.inst;
                addr = sit->second.base;
            } else if (auto *cr = Is<Call>(dt->obj); cr && cr->spec
                       && cr->spec->rets.size() == 1
                       && struct_ref_pointee(cr->spec->rets[0])) {
                // A call handing a reference to a struct back: the pointer
                // it yields is the path's base.
                auto *pv = EmitDirectCall(cr, w, nullptr);
                if (!pv) return false;
                inst = struct_ref_pointee(cr->spec->rets[0])->struc->inst;
                addr = pv;
            } else if (auto *ir = Is<InlineBlock>(dt->obj); ir && ir->spec
                       && ir->spec->rets.size() == 1
                       && struct_ref_pointee(ir->spec->rets[0])) {
                auto *pv = GenInline(ir, w);
                if (!pv) return false;
                inst = struct_ref_pointee(ir->spec->rets[0])->struc->inst;
                addr = pv;
            } else {
                // A struct value that is not a local: a call's result or a
                // literal. It is built in a temporary of its own, whose
                // address the path walks.
                llvm::Value *sv = nullptr;
                StructInst *si = nullptr;
                if (!struct_value(dt->obj, w, sv, si)) return false;
                auto *tmp = entry_alloca(w, struct_ll_ty(si), "stpath");
                b.CreateStore(sv, tmp);
                inst = si;
                addr = tmp;
            }
        }
        auto f = (unsigned)dt->fieldidx;
        if (f >= inst->ftypes.size()) return false;
        leaf = inst->ftypes[f];
        addr = b.CreateStructGEP(struct_ll_ty(inst), addr, f, "fldp");
        return true;
    }

    // A field read: the leaf's value at its own width, widened to the walk's
    // i64 (a nested struct comes back as the whole value, for the caller's
    // own struct path).
    llvm::Value *GenFieldRead(Dot *dt, FnWalk &w) {
        llvm::Value *addr = nullptr;
        StructInst *inst = nullptr;
        TypeExpr *leaf = nullptr;
        if (!field_path_addr(dt, w, addr, inst, leaf)) return nullptr;
        if (is_flat_struct(leaf))
            return b.CreateLoad(struct_ll_ty(leaf->struc->inst), addr, "stfld");
        auto *et = int_ll_ty(leaf);
        auto *v = b.CreateLoad(et, addr, "fld");
        if (et == i64()) return v;
        return is_uint_ty(leaf) ? b.CreateZExt(v, i64()) : b.CreateSExt(v, i64());
    }

    void GenFieldStore(Assign *as, Dot *dt, FnWalk &w) {
        llvm::Value *addr = nullptr;
        StructInst *inst = nullptr;
        TypeExpr *leaf = nullptr;
        if (!field_path_addr(dt, w, addr, inst, leaf))
            return Unsup("a field store into a value the walk does not hold");
        if (is_flat_struct(leaf)) {
            if (as->op != T_ASSIGN)
                return Unsup("a compound assignment to a struct field");
            llvm::Value *sv = nullptr;
            StructInst *si = nullptr;
            if (!struct_value(as->rhs, w, sv, si))
                return Unsup("a struct value the walk does not build");
            b.CreateStore(sv, addr);
            return;
        }
        auto *rhs = GenExpr(as->rhs, w);
        if (!rhs) return;
        auto *et = int_ll_ty(leaf);
        if (as->op != T_ASSIGN) {
            auto *cur = b.CreateLoad(et, addr, "fcur");
            auto *cur64 = is_uint_ty(leaf) ? b.CreateZExt(cur, i64())
                                           : b.CreateSExt(cur, i64());
            rhs = ArithEq(as->op, cur64, rhs, w, is_uint_ty(leaf));
            if (!rhs) return Unsup("a compound field assignment of this operator");
        }
        b.CreateStore(et == i64() ? rhs : b.CreateTrunc(rhs, et), addr);
    }

    // The value a compound assignment stores: `cur op rhs`, null for an
    // operator the walk does not model. `uns` says the target's storage is
    // unsigned, which changes `/` and `%` (and nothing else here).
    llvm::Value *ArithEq(TType op, llvm::Value *cur, llvm::Value *rhs,
                         FnWalk &w, bool uns = false) {
        switch (op) {
            case T_PLUSEQ:  return b.CreateAdd(cur, rhs, "pluseq");
            case T_MINUSEQ: return b.CreateSub(cur, rhs, "minuseq");
            case T_MULEQ:   return b.CreateMul(cur, rhs, "muleq");
            case T_DIVEQ:   return GenDivMod(T_DIV, cur, rhs, w, uns);
            case T_MODEQ:   return GenDivMod(T_MOD, cur, rhs, w, uns);
            case T_ANDEQ:   return b.CreateAnd(cur, rhs, "andeq");
            case T_OREQ:    return b.CreateOr(cur, rhs, "oreq");
            case T_XOREQ:   return b.CreateXor(cur, rhs, "xoreq");
            case T_SHLEQ:
                return b.CreateShl(cur,
                    b.CreateAnd(rhs, llvm::ConstantInt::get(i64(), 63), "shn"), "shleq");
            case T_SHREQ:
                return b.CreateAShr(cur,
                    b.CreateAnd(rhs, llvm::ConstantInt::get(i64(), 63), "shn"), "shreq");
            default:        return nullptr;
        }
    }

    // `a[i] = v` or `a[i] op= v` on a growable or fixed int array local (or a
    // growable global): the index is checked against the live count (fixed
    // arrays against their constant one), then the element is stored at its
    // native width.
    void GenIndexStore(Assign *as, Index *ix, FnWalk &w) {
        auto *obj = Is<Ident>(ix->obj);
        auto fit = obj && obj->vdef ? w.fixeds.find(obj->vdef) : w.fixeds.end();
        auto lit = obj && obj->vdef ? w.limiteds.find(obj->vdef) : w.limiteds.end();
        auto *r = (obj && obj->vdef && fit == w.fixeds.end() && lit == w.limiteds.end())
            ? rz_head(w, obj->vdef) : nullptr;
        SliceVal sv;
        bool isslice = false;
        if (fit == w.fixeds.end() && lit == w.limiteds.end() && !r) {
            // A view: a slice local, or a range of an array or view. The
            // subject expression is read out once.
            isslice = slice_value(ix->obj, w, sv);
            if (!isslice)
                return Unsup("an index assignment into an array the walk does not hold");
        }
        auto *idx = GenExpr(ix->idx, w);
        if (!idx) return;
        auto *rhs = GenExpr(as->rhs, w);
        if (!rhs) return;
        auto check = [&](llvm::Value *limit) {
            if (ix->nobc) return;
            auto *okbb = llvm::BasicBlock::Create(ctx, "stok", w.fn);
            auto *failbb = llvm::BasicBlock::Create(ctx, "stfail", w.fn);
            b.CreateCondBr(b.CreateICmpULT(idx, limit, "stin"), okbb, failbb);
            b.SetInsertPoint(failbb);
            b.CreateCall(f_gs_idxfail, { idx, limit, llvm::ConstantPointerNull::get(i8ptr()),
                                         llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 0) });
            b.CreateUnreachable();
            b.SetInsertPoint(okbb);
        };
        if (isslice) {
            auto *et = int_ll_ty(sv.etype);
            check(sv.len);
            auto *p = b.CreateInBoundsGEP(et, sv.data, idx, "stp");
            if (as->op != T_ASSIGN) {
                auto *cur = b.CreateLoad(et, p, "stcur");
                auto *cur64 = is_uint_ty(sv.etype) ? b.CreateZExt(cur, i64())
                                                   : b.CreateSExt(cur, i64());
                rhs = ArithEq(as->op, cur64, rhs, w, is_uint_ty(sv.etype));
                if (!rhs) return Unsup("a compound index assignment of this operator");
            }
            b.CreateStore(et == i64() ? rhs : b.CreateTrunc(rhs, et), p);
            return;
        }
        if (fit != w.fixeds.end()) {
            auto &fx = fit->second;
            auto *et = int_ll_ty(fx.etype);
            check(llvm::ConstantInt::get(i64(), (uint64_t)fx.count));
            if (!fx.base) return;
            auto *p = b.CreateInBoundsGEP(fx.base->getAllocatedType(), fx.base,
                                          { llvm::ConstantInt::get(i64(), 0), idx }, "stp");
            if (as->op != T_ASSIGN) {
                auto *cur = b.CreateLoad(et, p, "stcur");
                auto *cur64 = is_uint_ty(fx.etype) ? b.CreateZExt(cur, i64())
                                                   : b.CreateSExt(cur, i64());
                rhs = ArithEq(as->op, cur64, rhs, w, is_uint_ty(fx.etype));
                if (!rhs) return Unsup("a compound index assignment of this operator");
            }
            b.CreateStore(et == i64() ? rhs : b.CreateTrunc(rhs, et), p);
            return;
        }
        if (lit != w.limiteds.end()) {
            auto &lm = lit->second;
            auto *et = int_ll_ty(lm.etype);
            auto *lenv = b.CreateLoad(i64(), lm.len, "limlen");
            check(lenv);
            auto *p = b.CreateInBoundsGEP(lm.base->getAllocatedType(), lm.base,
                { llvm::ConstantInt::get(i64(), 0), idx }, "stip");
            if (as->op != T_ASSIGN) {
                auto *cur = b.CreateLoad(et, p, "stcur");
                auto *cur64 = is_uint_ty(lm.etype) ? b.CreateZExt(cur, i64())
                                                   : b.CreateSExt(cur, i64());
                rhs = ArithEq(as->op, cur64, rhs, w, is_uint_ty(lm.etype));
                if (!rhs) return Unsup("a compound index assignment of this operator");
            }
            b.CreateStore(et == i64() ? rhs : b.CreateTrunc(rhs, et), p);
            return;
        }
        auto *basev = b.CreateLoad(i8ptr(), r->base, "stbase");
        auto *lenv = b.CreateLoad(i64(), r->len, "stlen");
        check(lenv);
        if (as->op != T_ASSIGN) {
            auto *cur = rz_load_elem(*r, basev, idx, "stcur");
            rhs = ArithEq(as->op, cur, rhs, w, is_uint_ty(r->etype));
            if (!rhs) return Unsup("a compound index assignment of this operator");
        }
        rz_store_elem(*r, basev, idx, rhs);
    }

    // A direct call of a Goose function. Results 1.. are out-pointers the
    // callee writes (`outs[i]`, a pointer per result); result 0 is the value
    // returned. A multi-result call needs `outs`, a value context does not.
    llvm::Value *EmitDirectCall(Call *c, FnWalk &w,
                                const std::vector<llvm::Value *> *outs,
                                FnWalk::RzArr *growret = nullptr) {
        auto *sp = c->spec;
        if (!sp) return nullptr;
        for (auto *t : sp->argtypes)
            if (!is_int_ty(t) && !is_bool_ty(t) && !is_slice_of_int(t)
                && !is_flat_struct(t)
                && !(t && t->kind == TY_REF
                     && (is_int_ty(t->ref->sub)
                         || is_growable_i64(t->ref->sub)
                         || is_flat_struct(t->ref->sub)))) return nullptr;
        for (auto *rt : sp->rets)
            if (!is_int_ty(rt) && !is_bool_ty(rt) && !is_slice_of_int(rt)
                && !is_flat_struct(rt) && !is_flat_ref(rt)
                && !(sp->rets.size() == 1 && is_growable_i64(rt))) return nullptr;
        bool growret_ = sp->rets.size() == 1 && is_growable_i64(sp->rets[0]);
        if (growret_ && !growret) return nullptr;
        auto nrets = sp->rets.size();
        // A multi-result call in a value position (not bound to locals)
        // discards results 1..: it materializes their out-slots here and
        // returns only the value.
        std::vector<llvm::Value *> localouts;
        if (nrets > 1 && (!outs || outs->size() < nrets)) {
            localouts.assign(nrets, nullptr);
            for (size_t i = 1; i < nrets; ++i)
                localouts[i] = entry_alloca(w, i64(),
                                            cat("mr", (long long)i));
            outs = &localouts;
        }
        string callee = sp->sf->cname.empty() ? string(sp->sf->name) : sp->sf->cname;
        if (name_counts[sp->sf->qname] > 1)
            callee += cat("_", sp->id);
        auto *cf = mod->getFunction(callee);
        if (!cf) return nullptr;
        auto expect = c->args.size() + (nrets > 1 ? nrets - 1 : 0);
        // A reference to a growable array crosses a call as two pointers;
        // a growable result takes the (dst, rl) pair.
        for (auto *t : sp->argtypes)
            if (t && t->kind == TY_REF && is_growable_i64(t->ref->sub))
                expect += 1;
        if (growret_) expect += 2;
        if (cf->getFunctionType()->getNumParams() != expect) return nullptr;
        std::vector<llvm::Value *> args;
        for (size_t ai = 0; ai < c->args.size(); ++ai) {
            auto *a = c->args[ai];
            auto *pt = ai < sp->argtypes.size() ? sp->argtypes[ai] : nullptr;
            if (is_slice_of_int(pt)) {
                SliceVal sv;
                if (!slice_value(a, w, sv)) return nullptr;
                auto *hdr = b.CreateInsertValue(
                    llvm::UndefValue::get(slice_ll_ty()), sv.data, 0);
                args.push_back(b.CreateInsertValue(hdr, sv.len, 1));
                continue;
            }
            if (is_flat_struct(pt)) {
                llvm::Value *sv = nullptr;
                StructInst *si = nullptr;
                if (!struct_value(a, w, sv, si) || si != pt->struc->inst)
                    return nullptr;
                args.push_back(sv);
                continue;
            }
            if (pt && pt->kind == TY_REF && is_growable_i64(pt->ref->sub)) {
                // A reference to a growable array passes the pair naming
                // the array: a local's or a global's own header and stack,
                // or a reference's pair loaded from its holder.
                auto *a2 = a;
                if (auto *u = Is<Unary>(a2))
                    if (u->op == T_BITAND) a2 = u->child;
                auto *id = Is<Ident>(a2);
                llvm::Value *hp = nullptr;
                llvm::Value *spv = nullptr;
                if (!id || !id->vdef || !growable_pair(w, id->vdef, hp, spv))
                    return nullptr;
                args.push_back(hp);
                args.push_back(spv);
                continue;
            }
            if (pt->kind == TY_REF && is_flat_struct(pt->ref->sub)) {
                auto *ptr = ref_addr(a, w);
                if (!ptr) return nullptr;
                args.push_back(ptr);
                continue;
            }
            if (pt->kind == TY_REF && is_int_ty(pt->ref->sub)) {
                // A reference parameter's argument is the place it names:
                // a variable this walk holds, whose slot is passed, or a
                // reference, whose stored pointer is.
                auto *a2 = a;
                if (auto *u = Is<Unary>(a2))
                    if (u->op == T_BITAND) a2 = u->child;
                auto *id = Is<Ident>(a2);
                llvm::Value *ptr = nullptr;
                if (id && id->vdef) {
                    auto lit = w.locals.find(id->vdef);
                    if (lit != w.locals.end()) ptr = lit->second;
                    else {
                        auto gvit = global_vars.find(id->vdef);
                        if (gvit != global_vars.end()) ptr = gvit->second.gv;
                        else {
                            auto rit = w.refs.find(id->vdef);
                            if (rit != w.refs.end())
                                ptr = b.CreateLoad(i8ptr(), rit->second.base,
                                                   "refp");
                        }
                    }
                }
                if (!ptr) return nullptr;
                args.push_back(ptr);
                continue;
            }
            auto *av = GenExpr(a, w);
            if (!av) return nullptr;
            args.push_back(as_i64(av));
        }
        for (size_t i = 1; i < nrets; ++i) args.push_back((*outs)[i]);
        if (growret_) {
            args.push_back(growret->slot);
            args.push_back(growret->len);
        }
        return b.CreateCall(cf, args);
    }

    // The { data, len } an inlined callee's slice result is made of.
    llvm::Value *slice_struct(llvm::Value *data, llvm::Value *len) {
        auto *hdr = b.CreateInsertValue(llvm::UndefValue::get(slice_ll_ty()),
                                        data, 0);
        return b.CreateInsertValue(hdr, len, 1);
    }

    // An inlined call body: its statements, then its tail value. Each return
    // of the callee (Return above) branches here with its value; the values
    // meet in a PHI. A fallthrough with no value, or no value at all, gives
    // no result.
    llvm::Value *GenInline(InlineBlock *ib, FnWalk &w,
                           FnWalk::RzArr *growdst = nullptr) {
        auto *merge = llvm::BasicBlock::Create(ctx, "inlmerge", w.fn);
        w.inl.push_back({ ib->sf, ib->spec, merge, {}, growdst });
        size_t ei = w.inl.size() - 1;
        bool growret = growdst && ib->spec && ib->spec->rets.size() == 1
                    && is_growable_i64(ib->spec->rets[0]);
        bool sret = ib->spec && ib->spec->rets.size() == 1
                 && is_slice_of_int(ib->spec->rets[0]);
        bool stret = ib->spec && ib->spec->rets.size() == 1
                  && is_flat_struct(ib->spec->rets[0]);
        bool refret = ib->spec && ib->spec->rets.size() == 1
                   && is_flat_ref(ib->spec->rets[0]);
        llvm::Value *tv = nullptr;
        if (ib->body) {
            for (auto *s : ib->body->stmts) GenStmt(s, w);
            // The body's tail is its value, unless its type is void: then it
            // is the last statement (a loop or a return), not a value.
            Node *tn = ib->body->tail;
            if (tn && growret) {
                // The body ends in the value it returns: build its run.
                emit_growable_result(tn, growdst->slot, growdst->len,
                                     ib->spec->rets[0], w);
            } else if (tn) {
                bool valueless = !tn->exprtype || tn->exprtype->kind == TY_VOID;
                if (sret) {
                    SliceVal sv;
                    if (slice_value(tn, w, sv)) tv = slice_struct(sv.data, sv.len);
                } else if (stret) {
                    StructInst *si = nullptr;
                    llvm::Value *sv = nullptr;
                    if (struct_value(tn, w, sv, si)) tv = sv;
                } else if (refret) {
                    // An inlined body returning a reference: its tail names
                    // the address the call would have handed back.
                    tv = ref_addr(tn, w);
                } else if (valueless || Is<Return>(tn)) {
                    GenStmt(tn, w);
                } else {
                    tv = GenExpr(tn, w);
                }
            }
        }
        auto *cur = b.GetInsertBlock();
        // A block after a return that no edge reaches is dead: it adds no
        // edge to the merge.
        bool reach = !cur->getTerminator()
            && (cur == &w.fn->getEntryBlock() || !cur->hasNPredecessors(0));
        bool fell = false;
        if (reach) {
            if (tv) w.inl[ei].incoming.push_back({ tv, cur });
            else fell = true;
            b.CreateBr(merge);
        }
        auto incoming = std::move(w.inl[ei].incoming);
        w.inl.pop_back();
        b.SetInsertPoint(merge);
        if (fell || incoming.empty()) return nullptr;
        auto *ty = incoming[0].first->getType();
        for (auto &[v, bb] : incoming)
            if (v->getType() != ty) return nullptr;
        auto *phi = b.CreatePHI(ty, (unsigned)incoming.size(), "inl");
        for (auto &[v, bb] : incoming) phi->addIncoming(v, bb);
        return phi;
    }

    void GenStmt(Node *n, FnWalk &w) {
        if (!n) return;
        if (auto *blk = Is<Block>(n)) {
            // Only the walked function's own body ends in an implicit return;
            // a nested block's tail value flows back to the enclosing
            // expression, not a ret.
            auto top = blk == w.body;
            for (auto *s : blk->stmts) GenStmt(s, w);
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
                // A reference result: the body's tail names the address
                // the function hands back.
                if (top && w.fn->getReturnType()->isPointerTy() && w.spec
                    && w.spec->rets.size() == 1
                    && is_flat_ref(w.spec->rets[0])
                    && !b.GetInsertBlock()->getTerminator()) {
                    if (auto *p = ref_addr(blk->tail, w)) b.CreateRet(p);
                    return;
                }
                // A value-producing tail of the top-level body: the implicit
                // return. Nested blocks evaluate their tail for effect and
                // value only.
                auto *tv = GenExpr(blk->tail, w);
                // The implicit return of the walked function's own body: the
                // tail's value when its type is the shell's result type (an
                // int; a slice's { data, len }).
                if (top && tv && !b.GetInsertBlock()->getTerminator()
                    && tv->getType() == w.fn->getReturnType())
                    b.CreateRet(tv);
            }
            return;
        }
        if (auto *ib = Is<InlineBlock>(n)) {
            GenInline(ib, w);
            return;
        }
        if (auto *vd = Is<VarDecl>(n)) {
            // `let h, m = f(x)`: one multi-result call wired into the locals.
            // Result 0 is stored from the call's value, the others are the
            // out-pointers the callee writes.
            if (vd->defs.size() > 1 && vd->inits.size() == 1 && !vd->isglobal) {
                auto *c = Is<Call>(vd->inits[0]);
                if (!c || !c->spec || c->spec->rets.size() != vd->defs.size()) return;
                std::vector<llvm::Value *> outs(vd->defs.size(), nullptr);
                std::vector<llvm::AllocaInst *> slots(vd->defs.size(), nullptr);
                for (size_t i = 0; i < vd->defs.size(); ++i) {
                    // A bool result slots like an int: the walk carries both
                    // as i64.
                    if (!vd->defs[i]
                        || (!is_int_ty(vd->defs[i]->type)
                            && !is_bool_ty(vd->defs[i]->type))) return;
                    slots[i] = entry_alloca(w, i64(), llvm::Twine(vd->defs[i]->name));
                    outs[i] = slots[i];
                }
                auto *v0 = EmitDirectCall(c, w, &outs);
                if (!v0) return;
                b.CreateStore(v0, slots[0]);
                for (size_t i = 0; i < vd->defs.size(); ++i)
                    w.locals[vd->defs[i]] = slots[i];
                return;
            }
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
                if (vddef->type->kind == TY_ARRAY
                    && (vddef->type->arr->akind == A_FIXED || vddef->type->arr->akind == A_VAR)
                    && is_int_ty(vddef->type->arr->sub)) {
                    emit_fixed_decl(vd, i, *vddef, w);
                    continue;
                }
                if (vddef->type->kind == TY_ARRAY
                    && vddef->type->arr->akind == A_LIMITED
                    && is_int_ty(vddef->type->arr->sub)) {
                    emit_limited_decl(vd, i, *vddef, w);
                    continue;
                }
                if (is_flat_struct(vddef->type)) {
                    emit_struct_decl(vd, i, *vddef, w);
                    continue;
                }
                if (is_slice_of_int(vddef->type)) {
                    emit_slice_decl(vd, i, *vddef, w);
                    continue;
                }
                if (vddef->type->kind == TY_REF
                    && (is_int_ty(vddef->type->ref->sub)
                        || is_growable_i64(vddef->type->ref->sub)
                        || is_flat_struct(vddef->type->ref->sub))) {
                    emit_ref_decl(vd, i, *vddef, w);
                    continue;
                }
                if (!is_int_ty(vddef->type) && !is_bool_ty(vddef->type))
                    return Unsup(cat("a local of ", type_label(vddef->type),
                                     " at line ", (long long)vd->line.line));
                auto *alloca = entry_alloca(w, i64(), llvm::Twine(vddef->name));
                if (i < vd->inits.size() && vd->inits[i]) {
                    auto *initv = GenExpr(vd->inits[i], w);
                    if (initv)
                        b.CreateStore(widen_store(vddef->type, as_i64(initv)),
                                      alloca);
                }
                w.locals[vddef] = alloca;
            }
            return;
        }
        if (auto *as = Is<Assign>(n)) {
            if (auto *ix = Is<Index>(as->lval)) {
                GenIndexStore(as, ix, w);
                return;
            }
            if (auto *fd = Is<Dot>(as->lval)) {
                GenFieldStore(as, fd, w);
                return;
            }
            // i64 assignment to a local.
            auto *lv = Is<Ident>(as->lval);
            if (!lv || !lv->vdef) return Unsup("an assignment to a place the walk does not model");
            // Copying a flat int struct local: the whole value.
            if (auto stit = w.structs.find(lv->vdef); stit != w.structs.end()) {
                if (as->op != T_ASSIGN)
                    return Unsup("a compound assignment to a struct");
                llvm::Value *sv = nullptr;
                StructInst *si = nullptr;
                if (!struct_value(as->rhs, w, sv, si))
                    return Unsup("a struct value the walk does not build");
                b.CreateStore(sv, stit->second.base);
                return;
            }
            // Re-pointing a slice local: the whole { data, len } view.
            if (auto sit = w.slices.find(lv->vdef); sit != w.slices.end()) {
                if (as->op != T_ASSIGN)
                    return Unsup("a compound assignment to a slice");
                SliceVal sv;
                if (!slice_value(as->rhs, w, sv))
                    return Unsup("a slice value the walk does not build");
                auto *hdr = b.CreateInsertValue(
                    b.CreateInsertValue(llvm::UndefValue::get(slice_ll_ty()),
                                        sv.data, 0), sv.len, 1);
                b.CreateStore(hdr, sit->second.base);
                return;
            }
            // A reference to a growable array: a rebind re-points its pair,
            // which every later use loads.
            if (auto rrit = w.rzrefs.find(lv->vdef); rrit != w.rzrefs.end()) {
                if (as->op != T_DOTASSIGN)
                    return Unsup("an assignment through a reference to a "
                                 "growable array");
                auto *rhs = as->rhs;
                if (auto *u = Is<Unary>(rhs))
                    if (u->op == T_BITAND)
                        rhs = u->child;
                auto *rid = Is<Ident>(rhs);
                llvm::Value *hp = nullptr;
                llvm::Value *sp = nullptr;
                if (!rid || !rid->vdef || !growable_pair(w, rid->vdef, hp, sp))
                    return Unsup(cat("a rebind to a value the walk does "
                                     "not hold at line ",
                                     (long long)as->line.line));
                auto *holder = rrit->second.holder;
                b.CreateStore(hp, b.CreateStructGEP(rzref_ll_ty(), holder, 0));
                b.CreateStore(sp, b.CreateStructGEP(rzref_ll_ty(), holder, 1));
                return;
            }
            // A reference store goes into the pointee; `r .= e` (a rebind)
            // stores the new pointer instead, which the walk takes from a
            // variable it holds for now.
            if (auto rit = w.refs.find(lv->vdef); rit != w.refs.end()) {
                auto *p = b.CreateLoad(i8ptr(), rit->second.base, "refp");
                if (as->op == T_DOTASSIGN) {
                    auto *rhs = as->rhs;
                    // The checker wraps a rebinding rhs in a synthetic `&`
                    // (CheckRebind): the new pointer is what it covers.
                    if (auto *u = Is<Unary>(rhs))
                        if (u->op == T_BITAND)
                            rhs = u->child;
                    auto *rid = Is<Ident>(rhs);
                    llvm::Value *nptr = nullptr;
                    if (rid && rid->vdef) {
                        auto lit = w.locals.find(rid->vdef);
                        if (lit != w.locals.end()) nptr = lit->second;
                        else {
                            auto gvit = global_vars.find(rid->vdef);
                            if (gvit != global_vars.end()) nptr = gvit->second.gv;
                        }
                    }
                    if (!nptr)
                        return Unsup(cat("a rebind to a value the walk does "
                                         "not hold at line ",
                                         (long long)as->line.line));
                    b.CreateStore(nptr, rit->second.base);
                    return;
                }
                auto *rhs = as_i64(GenExpr(as->rhs, w));
                if (!rhs) return;
                auto *et = int_ll_ty(rit->second.ptee);
                if (as->op != T_ASSIGN) {
                    auto *cur = b.CreateLoad(et, p, "refcur");
                    auto *cur64 = is_uint_ty(rit->second.ptee)
                        ? b.CreateZExt(cur, i64()) : b.CreateSExt(cur, i64());
                    rhs = ArithEq(as->op, cur64, rhs, w, is_uint_ty(rit->second.ptee));
                    if (!rhs) return Unsup("a compound assignment of this operator");
                }
                b.CreateStore(et == i64() ? rhs : b.CreateTrunc(rhs, et), p);
                return;
            }
            auto *rhs = as_i64(GenExpr(as->rhs, w));
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
                        auto *alloca = entry_alloca(w, i64(), llvm::Twine(lv->vdef->name));
                        b.CreateStore(pit->second, alloca);
                        w.locals[lv->vdef] = alloca;
                        cur = pit->second;
                    } else {
                        auto lit = w.locals.find(lv->vdef);
                        if (lit == w.locals.end()) return;
                        cur = b.CreateLoad(i64(), lit->second, llvm::Twine(lv->vdef->name));
                    }
                }
                rhs = ArithEq(as->op, cur, rhs, w, is_uint_ty(lv->exprtype));
                if (!rhs) return Unsup("a compound assignment of this operator");
            }
            auto pit2 = w.params.find(lv->vdef);
            auto lit2 = w.locals.find(lv->vdef);
            auto gv2 = global_vars.find(lv->vdef);
            if (gv2 != global_vars.end()) {
                b.CreateStore(widen_store(lv->vdef->type, rhs), gv2->second.gv);
            } else if (pit2 != w.params.end()) {
                // Plain assign to a parameter: store into a shadow alloca.
                if (lit2 == w.locals.end()) {
                    auto *alloca = entry_alloca(w, i64(), llvm::Twine(lv->vdef->name));
                    b.CreateStore(pit2->second, alloca);
                    w.locals[lv->vdef] = alloca;
                    lit2 = w.locals.find(lv->vdef);
                }
                b.CreateStore(widen_store(lv->vdef->type, rhs), lit2->second);
            } else if (lit2 != w.locals.end()) {
                b.CreateStore(widen_store(lv->vdef->type, rhs), lit2->second);
            } else {
                Unsup("an assignment to a variable the walk does not hold");
            }
            return;
        }
        if (auto *le = Is<LoopExpr>(n)) {
            // `loop { ... }`: an unconditional loop that leaves only by the
            // breaks in its body. A loop expression carrying a value (a
            // `break v` that names it) is a later cut.
            if (le->exprtype && le->exprtype->kind != TY_VOID)
                return Unsup(cat("a loop expression of a value at line ",
                                 (long long)le->line.line));
            auto *fn = w.fn;
            auto *bodybb = llvm::BasicBlock::Create(ctx, "loop_body", fn);
            auto *donebb = llvm::BasicBlock::Create(ctx, "loop_done", fn);
            w.loops.push_back({bodybb, donebb, bodybb});
            b.CreateBr(bodybb);
            b.SetInsertPoint(bodybb);
            if (le->body) GenStmt(le->body, w);
            if (!b.GetInsertBlock()->getTerminator()) b.CreateBr(bodybb);
            w.loops.pop_back();
            b.SetInsertPoint(donebb);
            return;
        }
        if (auto *wh = Is<While>(n)) {
            auto *fn = w.fn;
            auto *condbb = llvm::BasicBlock::Create(ctx, "while_cond", fn);
            auto *bodybb = llvm::BasicBlock::Create(ctx, "while_body", fn);
            auto *donebb = llvm::BasicBlock::Create(ctx, "while_done", fn);
            w.loops.push_back({condbb, donebb, condbb});
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
                // A range is two bounds, not a value: the header is not a
                // RangeExpr expression the walk evaluates.
                llvm::Value *lo = llvm::ConstantInt::get(i64(), 0);
                llvm::Value *hi = nullptr;
                if (auto *r = Is<RangeExpr>(fl->iter)) {
                    lo = GenExpr(r->lo, w);
                    hi = GenExpr(r->hi, w);
                } else {
                    hi = GenExpr(fl->iter, w);
                }
                if (!lo || !hi)
                    return Unsup(cat("a for header at line ",
                                     (long long)fl->line.line));
                auto *fn = w.fn;
                auto *condbb = llvm::BasicBlock::Create(ctx, "for_cond", fn);
                auto *bodybb = llvm::BasicBlock::Create(ctx, "for_body", fn);
                auto *donebb = llvm::BasicBlock::Create(ctx, "for_done", fn);
                // The counter's step runs in its own block: a `continue`
                // lands there, so it steps exactly as a fallthrough does.
                auto *incbb = llvm::BasicBlock::Create(ctx, "for_inc", fn);
                w.loops.push_back({condbb, donebb, incbb});
                b.CreateBr(condbb);
                b.SetInsertPoint(condbb);
                auto *iv = b.CreatePHI(i64(), 2, "for_i");
                iv->addIncoming(lo, b.GetInsertBlock()->getSinglePredecessor());
                auto *cond = b.CreateICmpSLT(iv, hi, "for_test");
                b.CreateCondBr(cond, bodybb, donebb);
                b.SetInsertPoint(bodybb);
                if (fl->vdef) {
                    auto *slot = entry_alloca(w, i64(), llvm::Twine(fl->vdef->name));
                    b.CreateStore(iv, slot);
                    w.locals[fl->vdef] = slot;
                }
                if (fl->body) GenStmt(fl->body, w);
                auto *body_end = b.GetInsertBlock();
                if (!body_end->getTerminator()) b.CreateBr(incbb);
                b.SetInsertPoint(incbb);
                auto *next = b.CreateAdd(iv, llvm::ConstantInt::get(i64(), 1), "for_next");
                iv->addIncoming(next, incbb);
                b.CreateBr(condbb);
                w.loops.pop_back();
                b.SetInsertPoint(donebb);
                return;
            }
            if (fl->iterkind == IK_ARRAY || fl->iterkind == IK_SLICE) {
                // Element iteration over an int-element array: the binder
                // names the element read per pass, the optional index
                // binder its position, and the count re-reads each pass
                // for a growable (pushes during iteration stay legal, so
                // grown-into elements are seen by later passes); a fixed
                // array's count is the construction constant.
                auto *obj = Is<Ident>(fl->iter);
                FnWalk::RzArr *gr = nullptr;
                FnWalk::FixedArr *fa = nullptr;
                FnWalk::LimArr *la = nullptr;
                if (obj && obj->vdef) {
                    gr = rz_head(w, obj->vdef);
                    if (!gr) {
                        auto fit = w.fixeds.find(obj->vdef);
                        if (fit != w.fixeds.end()) fa = &fit->second;
                        else {
                            auto lit = w.limiteds.find(obj->vdef);
                            if (lit != w.limiteds.end()) la = &lit->second;
                        }
                    }
                }
                // A view: a slice local, a range of an array, or a range of a
                // view. A slice local is re-read each pass (its body may
                // re-point it, section 5.2); a range expression's view is the
                // temporary it made.
                SliceVal sv;
                bool isv = false;
                if (!gr && !fa && !la) {
                    isv = slice_value(fl->iter, w, sv);
                    if (!isv)
                        return Unsup(cat("a for loop over ",
                                         type_label(fl->iter->exprtype),
                                         " at line ", (long long)fl->line.line));
                }
                auto *iid = isv ? Is<Ident>(fl->iter) : nullptr;
                bool reload = iid && iid->vdef && w.slices.count(iid->vdef);
                TypeExpr *etype = gr ? gr->etype
                    : fa ? fa->etype : la ? la->etype : sv.etype;
                llvm::Type *et = int_ll_ty(etype);
                auto *fn = w.fn;
                auto *condbb = llvm::BasicBlock::Create(ctx, "for_cond", fn);
                auto *bodybb = llvm::BasicBlock::Create(ctx, "for_body", fn);
                auto *donebb = llvm::BasicBlock::Create(ctx, "for_done", fn);
                // The counter's step runs in its own block: a `continue`
                // lands there, so it steps exactly as a fallthrough does.
                auto *incbb = llvm::BasicBlock::Create(ctx, "for_inc", fn);
                w.loops.push_back({condbb, donebb, incbb});
                b.CreateBr(condbb);
                b.SetInsertPoint(condbb);
                auto *iv = b.CreatePHI(i64(), 2, "for_i");
                iv->addIncoming(llvm::ConstantInt::get(i64(), 0),
                                b.GetInsertBlock()->getSinglePredecessor());
                if (reload) {
                    SliceVal fresh;
                    if (slice_value(fl->iter, w, fresh)) sv = fresh;
                }
                llvm::Value *lenv = gr
                    ? (llvm::Value *)b.CreateLoad(i64(), gr->len, "for_len")
                    : fa ? (llvm::Value *)llvm::ConstantInt::get(i64(), (uint64_t)fa->count)
                    : la ? (llvm::Value *)b.CreateLoad(i64(), la->len, "for_len")
                         : sv.len;
                b.CreateCondBr(b.CreateICmpULT(iv, lenv, "for_test"),
                               bodybb, donebb);
                b.SetInsertPoint(bodybb);
                if (fl->vdef && fl->byref) {
                    // `for &x in xs`: the binder is a reference to the
                    // element, so the body's writes land in the array itself
                    // (the C backend binds the element's address).
                    llvm::Value *ep = fa
                        ? (fa->base
                            ? (llvm::Value *)b.CreateInBoundsGEP(
                                fa->base->getAllocatedType(), fa->base,
                                { llvm::ConstantInt::get(i64(), 0), iv },
                                "for_rp")
                            : (llvm::Value *)llvm::ConstantPointerNull::get(i8ptr()))
                        : gr ? (llvm::Value *)b.CreateInBoundsGEP(
                            et, b.CreateLoad(i8ptr(), gr->base), iv, "for_rp")
                        : la ? (llvm::Value *)b.CreateInBoundsGEP(
                            la->base->getAllocatedType(), la->base,
                            { llvm::ConstantInt::get(i64(), 0), iv }, "for_rp")
                             : (llvm::Value *)b.CreateInBoundsGEP(et, sv.data, iv,
                                                                  "for_rp");
                    auto *slot = entry_alloca(w, i8ptr(),
                                              llvm::Twine(fl->vdef->name));
                    b.CreateStore(ep, slot);
                    w.refs[fl->vdef] = { slot, etype };
                } else if (fl->vdef) {
                    auto *slot = entry_alloca(w, i64(),
                                              llvm::Twine(fl->vdef->name));
                    // A zero-count array has no alloca: its body never runs,
                    // and the binder's value is a typed zero.
                    llvm::Value *ev = fa
                        ? (fa->base
                            ? b.CreateLoad(et,
                                b.CreateInBoundsGEP(fa->base->getAllocatedType(),
                                    fa->base,
                                    { llvm::ConstantInt::get(i64(), 0), iv },
                                    "for_ip"), "for_ev")
                            : (llvm::Value *)llvm::Constant::getNullValue(et))
                        : gr ? rz_load_elem(*gr,
                            b.CreateLoad(i8ptr(), gr->base), iv, "for_ip")
                        : la ? b.CreateLoad(et,
                            b.CreateInBoundsGEP(la->base->getAllocatedType(),
                                la->base,
                                { llvm::ConstantInt::get(i64(), 0), iv },
                                "for_ip"), "for_ev")
                             : b.CreateLoad(et,
                                 b.CreateInBoundsGEP(et, sv.data, iv, "for_ip"),
                                 "for_ev");
                    b.CreateStore(et == i64() ? ev
                        : is_uint_ty(etype)
                            ? b.CreateZExt(ev, i64())
                            : b.CreateSExt(ev, i64()), slot);
                    w.locals[fl->vdef] = slot;
                }
                if (fl->idxdef) {
                    auto *slot = entry_alloca(w, i64(),
                                              llvm::Twine(fl->idxdef->name));
                    b.CreateStore(iv, slot);
                    w.locals[fl->idxdef] = slot;
                }
                if (fl->body) GenStmt(fl->body, w);
                auto *body_end = b.GetInsertBlock();
                if (!body_end->getTerminator()) b.CreateBr(incbb);
                b.SetInsertPoint(incbb);
                auto *next = b.CreateAdd(iv, llvm::ConstantInt::get(i64(), 1), "for_next");
                iv->addIncoming(next, incbb);
                b.CreateBr(condbb);
                w.loops.pop_back();
                b.SetInsertPoint(donebb);
                return;
            }
            return Unsup(cat("a for loop over ", type_label(fl->iter->exprtype),
                             " at line ", (long long)fl->line.line));
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
            // A `break` out of an EarlyBlock is not modeled in the minimal
            // walk yet (its value would flow out of the block): left out.
            return;
        }
        if (auto *cont = Is<Continue>(n)) {
            if (w.loops.empty()) return;
            auto &lp = w.loops.back();
            b.CreateBr(lp.contbb ? lp.contbb : lp.condbb);
            return;
        }
        if (auto *inc = Is<IncDec>(n)) {
            auto *lv = Is<Ident>(inc->lval);
            if (!lv || !lv->vdef) return;
            auto rit = w.refs.find(lv->vdef);
            if (rit != w.refs.end()) {
                // A step of the reference's pointee, through its pointer.
                auto *p = b.CreateLoad(i8ptr(), rit->second.base, "refp");
                auto *et = int_ll_ty(rit->second.ptee);
                auto *cur = b.CreateLoad(et, p, llvm::Twine(lv->vdef->name));
                auto *one = llvm::ConstantInt::get(et, 1);
                auto *nv = inc->op == T_INC
                    ? b.CreateAdd(cur, one, "inc") : b.CreateSub(cur, one, "dec");
                b.CreateStore(nv, p);
                return;
            }
            auto lit = w.locals.find(lv->vdef);
            if (lit == w.locals.end()) {
                auto pit = w.params.find(lv->vdef);
                if (pit == w.params.end()) {
                    // A program global: its own storage, not a stack slot.
                    auto git = global_vars.find(lv->vdef);
                    if (git == global_vars.end())
                        return Unsup(cat("an increment of a variable the walk "
                                         "does not hold at line ",
                                         (long long)inc->line.line));
                    auto *one = llvm::ConstantInt::get(i64(), 1);
                    auto *cur = b.CreateLoad(i64(), git->second.gv,
                                             llvm::Twine(lv->vdef->name));
                    auto *nv = inc->op == T_INC ? b.CreateAdd(cur, one, "inc")
                                                : b.CreateSub(cur, one, "dec");
                    b.CreateStore(widen_store(lv->vdef->type, nv),
                                  git->second.gv);
                    return;
                }
                auto *alloca = entry_alloca(w, i64(), llvm::Twine(lv->vdef->name));
                b.CreateStore(pit->second, alloca);
                w.locals[lv->vdef] = alloca;
                lit = w.locals.find(lv->vdef);
            }
            auto *one = llvm::ConstantInt::get(i64(), 1);
            auto *cur = b.CreateLoad(i64(), lit->second, llvm::Twine(lv->vdef->name));
            auto *nv = inc->op == T_INC ? b.CreateAdd(cur, one, "inc")
                                        : b.CreateSub(cur, one, "dec");
            b.CreateStore(widen_store(lv->vdef->type, nv), lit->second);
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
                // An inlined callee's return: the value goes to the merge of
                // that body's InlineBlock, and the code after it is dead.
                size_t ei = w.inl.size();
                while (ei > 0 && w.inl[ei - 1].sf != ret->target) --ei;
                if (ei == 0) return;
                --ei;
                auto *ispec = w.inl[ei].spec;
                bool sret = ispec && ispec->rets.size() == 1
                         && is_slice_of_int(ispec->rets[0]);
                bool stret = ispec && ispec->rets.size() == 1
                          && is_flat_struct(ispec->rets[0]);
                bool refret = ispec && ispec->rets.size() == 1
                           && is_flat_ref(ispec->rets[0]);
                auto *growdst = w.inl[ei].growret;
                if (growdst && !ret->vals.empty()) {
                    // The inlined body's growable result: build its run into
                    // the destination the caller bound.
                    emit_growable_result(ret->vals[0], growdst->slot,
                                         growdst->len, ispec->rets[0], w);
                    b.CreateBr(w.inl[ei].merge);
                    b.SetInsertPoint(llvm::BasicBlock::Create(
                        ctx, "inlret.dead", w.fn));
                    return;
                }
                llvm::Value *v = nullptr;
                if (!ret->vals.empty()) {
                    if (sret) {
                        SliceVal sv;
                        if (!slice_value(ret->vals[0], w, sv)) return;
                        v = slice_struct(sv.data, sv.len);
                    } else if (stret) {
                        StructInst *si = nullptr;
                        if (!struct_value(ret->vals[0], w, v, si)) return;
                    } else if (refret) {
                        v = ref_addr(ret->vals[0], w);
                        if (!v) return;
                    } else {
                        v = GenExpr(ret->vals[0], w);
                        if (!v) return;
                    }
                }
                if (v) w.inl[ei].incoming.push_back({ v, b.GetInsertBlock() });
                b.CreateBr(w.inl[ei].merge);
                b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "inlret.dead", w.fn));
                return;
            }
            auto nrets = w.spec ? w.spec->rets.size() : (size_t)0;
            if (nrets > 1 && ret->vals.size() == 1) {
                // `return f(x)` forwarding every result: the callee writes
                // results 1.. straight into this function's out-pointers.
                auto *c = Is<Call>(ret->vals[0]);
                if (!c) return;
                std::vector<llvm::Value *> outs(nrets, nullptr);
                for (size_t i = 1; i < nrets; ++i)
                    outs[i] = w.fn->getArg(w.outstart + i - 1);
                auto *v0 = EmitDirectCall(c, w, &outs);
                if (v0) b.CreateRet(v0);
                return;
            }
            if (ret->vals.size() > 1) {
                // `return a, b, c`: all values first in source order, then
                // results 1.. stored through the out-pointers, result 0 returned.
                std::vector<llvm::Value *> vals;
                for (auto *vn : ret->vals) {
                    auto *v = GenExpr(vn, w);
                    if (!v) return;
                    vals.push_back(v);
                }
                // An out-pointer is written at the walk's own width: a bool
                // result stores as i64, not as the i1 its compare produced,
                // or the caller would read the slot's other bytes.
                for (size_t i = 1; i < vals.size(); ++i)
                    b.CreateStore(as_i64(vals[i]),
                                  w.fn->getArg(w.outstart + i - 1));
                b.CreateRet(as_i64(vals[0]));
                return;
            }
            if (w.spec && w.spec->rets.size() == 1
                && is_growable_i64(w.spec->rets[0])
                && w.fn->getReturnType()->isVoidTy()
                && ret->vals.size() == 1) {
                // The run form: the value to return is a growable the walk
                // holds; its elements are copied onto the caller's stack
                // (dst, rl are the last two arguments).
                auto *dst = w.fn->getArg(w.outstart);
                auto *rl = w.fn->getArg(w.outstart + 1);
                if (!emit_growable_result(ret->vals[0], dst, rl,
                                          w.spec->rets[0], w))
                    return Unsup(cat("a growable return the walk does not "
                                     "build at line ",
                                     (long long)ret->line.line));
                b.CreateRetVoid();
                return;
            }
            if (ret->vals.empty() && w.fn->getReturnType()->isVoidTy()) {
                b.CreateRetVoid();
                return;
            }
            if (!ret->vals.empty()) {
                if (w.fn->getReturnType() == slice_ll_ty()) {
                    SliceVal sv;
                    if (!slice_value(ret->vals[0], w, sv)) return;
                    auto *hdr = b.CreateInsertValue(
                        llvm::UndefValue::get(slice_ll_ty()), sv.data, 0);
                    b.CreateRet(b.CreateInsertValue(hdr, sv.len, 1));
                    return;
                }
                if (w.fn->getReturnType()->isStructTy()) {
                    llvm::Value *sv = nullptr;
                    StructInst *si = nullptr;
                    if (!struct_value(ret->vals[0], w, sv, si)) return;
                    b.CreateRet(sv);
                    return;
                }
                if (w.fn->getReturnType()->isPointerTy() && w.spec
                    && w.spec->rets.size() == 1
                    && is_flat_ref(w.spec->rets[0])) {
                    auto *p = ref_addr(ret->vals[0], w);
                    if (p) b.CreateRet(p);
                    return;
                }
                auto *v = GenExpr(ret->vals[0], w);
                if (v) b.CreateRet(as_i64(v));
                return;
            }
            // A value-returning function's bare `return;` defaults to zero
            // (the full walk uses the type's default).
            if (w.fn->getReturnType()->isIntegerTy(64))
                b.CreateRet(llvm::ConstantInt::get(i64(), 0));
            else if (w.fn->getReturnType() == slice_ll_ty())
                b.CreateRet(llvm::Constant::getNullValue(slice_ll_ty()));
            else if (w.fn->getReturnType()->isStructTy())
                b.CreateRet(llvm::Constant::getNullValue(
                    w.fn->getReturnType()));
            else if (w.fn->getReturnType()->isPointerTy())
                b.CreateRet(llvm::ConstantPointerNull::get(i8ptr()));
            return;
        }
        // Expression-as-statement (e.g. print(...)). A statement that is not
        // an expression (a defer, say) has no emission here.
        // Declarations of nested functions and types emit nothing here.
        auto decl = n->nkind == NK_FnDecl || n->nkind == NK_StructDecl
                 || n->nkind == NK_EnumDecl || n->nkind == NK_AliasDecl;
        if (!n->exprtype && !Is<Call>(n) && !decl) return Unsup(cat("a statement of kind ", (int)n->nkind,
                                                             " at line ", (long long)n->line.line));
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
                    auto *hdrgv = new llvm::GlobalVariable(
                        *mod, slice_ll_ty(), false,
                        llvm::GlobalValue::InternalLinkage,
                        llvm::ConstantStruct::get(
                            slice_ll_ty(),
                            { llvm::ConstantPointerNull::get(i8ptr()),
                              llvm::ConstantInt::get(i64ty, 0) }),
                        gname + ".hdr", nullptr,
                        llvm::GlobalValue::GeneralDynamicTLSModel);
                    llvm::Constant *bidx[2] = { idx0(), fld(0) };
                    llvm::Constant *lidx[2] = { idx0(), fld(1) };
                    auto *basegv =
                        llvm::ConstantExpr::getInBoundsGetElementPtr(
                            slice_ll_ty(), hdrgv, bidx);
                    auto *lengv =
                        llvm::ConstantExpr::getInBoundsGetElementPtr(
                            slice_ll_ty(), hdrgv, lidx);
                    global_rzs[d] = { basegv, lengv, slot, hdrgv,
                                      d->type->arr->sub };
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
