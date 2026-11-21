# LLVM-IR Codegen (M2–M6) Implementation Plan

> **Path note (2026-11-08):** all `src/codegen_ir*.h` paths below now live
> under `src/llvm/`, and `src/runtime/ir_stub.c` is `src/llvm/ir_stub.c`.
> Read path references with that substitution.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an LLVM-IR codegen backend to the Goose compiler, parallel to the C backend, that emits a valid LLVM module for each program, links against `libgoose_rt.a`, and produces a native executable whose stdout matches the C backend's output byte-for-byte.

**Architecture:** The pipeline (parse → dump → typecheck → optimize → bce) is shared. At the divergence point, a new `CodeGenIR` class (in `src/codegen_ir*.h`) walks the same `FnSpec` specializations that `CodeGen` (the C backend) walks, and emits LLVM IR through `IRBuilder` instead of a C string buffer. The emitted IR links against `libgoose_rt.a` (built by M1) and against the C adapter objects for `c_headers` packages. The `goose llc` driver (M5) picks the target triple + DataLayout, runs `llc` → `clang`, links the runtime archive, and runs the resulting executable. `test/ir_runner.py` is the fixture runner for the IR path.

**Tech stack:** C++20 (compiler), LLVM 15+ (`IRBuilder`, `LLVMContext`, `Module`, `GlobalVariable`, `Function`), Clang (compiling the runtime archive and the C adapters), `llc` (IR → assembly), `clang` (assembly + link → native exe). CMake ≥ 3.27.

**Spec:** `docs/superpowers/specs/2026-11-07-llvm-backend-design.md` (M2–M6 milestones), `docs/design/llvm_backend.md`.

## Global Constraints

- **LLVM minimum version 15.** `FindGooseLLVM` (M1, already in place) fails with a diagnostic if the system LLVM is < 15. All IR codegen assumes LLVM 15+ API.
- **`GOOSE_LLVM=on` is opt-in.** The C backend remains the default. All work in this plan is gated on `GOOSE_LLVM=on`; the default build (`GOOSE_LLVM=off`) must not be affected.
- **TLS model: `GeneralDynamicTLSModel` everywhere.** No per-platform `InitialExec` optimization (partner decision 3).
- **`c_headers` adapters: hand-translated IR declarations (option B).** The IR module `declare`s each adapter; the `goose llc` driver compiles the C adapter source into an object and links it. The four external packages (gfx, audio, ui, physics) remain unchanged in C.
- **Runtime asserts (`--stack-assert`) on by default in test-suite runs.** The IR codegen inserts `icmp + call abort()` at every bump site when the flag is set. Off in release builds (partner decision 2).
- **No changes to the C backend, parser, typecheck, optimizer, BCE, dump, LSP, or stdlib semantics.** The IR backend is a new leaf in the pipeline.
- **`thread_fn` is a separate function in the same module** (not a separate module). It is called via `gs_thread_spawn` and receives flat-copied arguments.
- **BCE runs before IR codegen.** IR codegen sees the BCE-reduced AST and emits exactly the checks BCE says to keep. The IR builder does not re-do BCE.
- **Test-suite parity: byte-for-byte stdout match** against `expected/<name>.out` for every fixture and sample that does not carry a `no-llvm` marker.
- **Commit messages:** one-line subject (≤ 72 chars, only "what changed"), blank line, then body ≤ 2 sentences. No lists, no diff retelling, no "why" paragraphs. WI/issue reference short, in subject or body.
- **ASCII + Cyrillic only in source files.** No emoji, no special Unicode.

## Review Focus

1. **TLS model correctness.** A wrong TLS model (`LocalExecTLSModel` vs `GeneralDynamicTLSModel`) causes `gs_stks`/`gs_gl` to resolve to the wrong per-thread copy under threads. Expected: all thread fixtures pass under `--backend ir`.
2. **Guard-gap not re-implemented in IR.** The guard-gap (`mmap` + `mprotect`, 1 MiB unmapped tail) lives in `libgoose_rt.a` (C). The IR module only *calls* `gs_reserve_region`; it does not emit `mmap`/`mprotect`. Expected: `nm libgoose_rt.a | grep reserve` shows the symbol; the IR module does not contain `@mmap` or `@mprotect` declarations.
3. **c_headers adapter linkage.** The IR module `declare`s the adapter; the `goose llc` driver must compile the C adapter and link it. Expected: a fixture that imports a package with `c_headers` (e.g. `packages/`) compiles and runs under `--backend ir`.
4. **`gs_sp` is i64, not i8*.** The C backend emits `int64_t gs_sp` (scalar, `codegen_frames.h:623`). The IR must use `i64`, not `i8*`. Expected: the emitted IR has `i64` for `gs_sp` arguments, not `i8*`.
5. **`--stack-assert` only in test-suite runs.** The flag must be off in release builds. Expected: `goose --stack-assert` produces IR with `icmp`/`abort` pairs at bump sites; `goose` without the flag does not.

---

### Task 1: Integrate `codegen_ir_types.h` and `codegen_ir_values.h` into the build

**Files:**
- Modify: `CMakeLists.txt:216-220` (remove the placeholder `target_sources` for the two header-only files; they are not standalone TUs)
- Create: `src/codegen_ir.h` (umbrella header that includes all IR codegen pieces; the real `CodeGenIR` class will live here in Task 2)

**Interfaces:**
- Consumes: M1 infrastructure (`libgoose_rt.a`, `FindGooseLLVM`, `LLVM_LIBS`, `LLVM_INCLUDE_DIRS`)
- Produces: `#include "codegen_ir.h"` (the umbrella) which is `#include`d from `src/main.cpp` when `GOOSE_LLVM` is on

- [ ] **Step 1: Remove the placeholder `target_sources` for the two header-only files from CMakeLists.txt**

The two header files are not standalone TUs; they are `#include`d from `main.cpp` (or from `codegen_ir.h`). Remove:

```cmake
    target_sources(goose PRIVATE
        src/codegen_ir_types.h
        src/codegen_ir_values.h)
```

- [ ] **Step 2: Create `src/codegen_ir.h` as the umbrella header**

```cpp
// LLVM-IR backend: umbrella header.
// Includes all IR codegen pieces in dependency order.
// #include from main.cpp when GOOSE_LLVM is on.
#pragma once
#include "codegen_ir_types.h"
#include "codegen_ir_values.h"
// M3+: frames, stacks, construct
// M4+: calls, emit, c_headers adapters
```

- [ ] **Step 3: Verify the default build (GOOSE_LLVM=off) is unaffected**

Run:
```bash
cd /mnt/e/work/goose && rm -rf build/CMakeCache.txt build/CMakeFiles
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/goose --version
```
Expected: `goose vX.Y.Z-dirty` prints without LLVM errors.

- [ ] **Step 4: Verify the LLVM build (GOOSE_LLVM=on) still compiles**

Run:
```bash
cd /mnt/e/work/goose && rm -rf build/CMakeCache.txt build/CMakeFiles
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGOOSE_LLVM=ON
cmake --build build -j$(nproc)
./build/goose --version
```
Expected: `goose vX.Y.Z-dirty` prints; no `codegen_ir*.h` errors.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/codegen_ir.h
git commit -m "codegen_ir: umbrella header and CMake wiring for GOOSE_LLVM=on"
```

---

### Task 2: `CodeGenIR` class — module setup, TLS globals, and extern declarations

**Files:**
- Create: `src/codegen_ir_main.h` (the `CodeGenIR` class: module setup, TLS global emission, extern declarations, and the `Run()` driver that walks the FnSpec list)

**Interfaces:**
- Consumes: `src/codegen_ir_types.h` (`IrModuleBuilder`, `IrStackLayout`), `src/codegen_ir_values.h` (`CodeGenIR` value-emitting methods — these become members of the class in this task), the `Ast` + `FnSpec` types from the pipeline
- Produces: `struct CodeGenIR { ... }` with `void Run()` and `llvm::Module &module()`

**Design note:** `CodeGenIR` mirrors the shape of `CodeGen` (`src/codegen.h:90`): it takes an `Ast &`, a list of `FnSpec *`s, and emits the whole program into one `llvm::Module`. The `Run()` method is the entry point called from `main.cpp` when `GOOSE_LLVM` is on. The class does **not** re-implement BCE — it sees the BCE-reduced AST.

- [ ] **Step 1: Define the `CodeGenIR` struct in `src/codegen_ir_main.h`**

```cpp
#pragma once
#include "codegen_ir.h"
#include "ast.h"
#include "typecheck.h"  // FnSpec

namespace goose {

struct CodeGenIR {
    Ast &ast;
    IrModuleBuilder m;
    llvm::IRBuilder<> b;

    // The FnSpec specializations to emit (same list CodeGen receives).
    vector<FnSpec *> specs;

    // Whether to insert --stack-assert checks at bump sites.
    bool stack_assert = false;

    // Construct: creates the module and emits TLS globals.
    CodeGenIR(Ast &a, vector<FnSpec *> &specs, bool stack_assert)
        : ast(a), m("goose_program") {
        this->specs = specs;
        this->stack_assert = stack_assert;
        b.Initialize(&m.ctx, m.ctx.getContextMD());
        m.gs_gl = nullptr;  // emit_tls_globals() fills these in
        // ...
    }

    // Emit the six TLS globals (GeneralDynamicTLSModel).
    void emit_tls_globals();

    // Declare all gs_* runtime functions (from libgoose_rt.a).
    void declare_runtime_externs();

    // Drive the emission: for each FnSpec, create the IR function and
    // walk its body. The full body-walk is M3+; for now this is a
    // stub that creates the function shells.
    void Run();

    llvm::Module &module() { return *m.mod; }
};

} // namespace goose
```

**Important:** `b.Initialize(...)` requires the `LLVMContext`'s module; use `b.SetDefaultInsertBlock(...)` or `IRBuilder` without `Initialize` (LLVM 15+ does not require `Initialize` — it takes the `IRBuilder()` default constructor and uses `IRBuilder<> b(mod->getContext())`). Adjust the constructor to:

```cpp
    CodeGenIR(Ast &a, vector<FnSpec *> &specs, bool stack_assert)
        : ast(a), m("goose_program"), b(m.mod->getContext()) {
        this->specs = specs;
        this->stack_assert = stack_assert;
        emit_tls_globals();
        declare_runtime_externs();
    }
```

- [ ] **Step 2: Implement `emit_tls_globals()` in `codegen_ir_main.h`**

Move the TLS global emission from `codegen_ir_values.h` (where it currently lives as a method on a struct) into `CodeGenIR::emit_tls_globals()`. The six globals are `@gs_gl`, `@gs_stks`, `@gs_nstks`, `@gs_current_thread_id`, `@gs_regions`, `@gs_regions_cap`, all `CommonLinkage`, `GeneralDynamicTLSModel`, with `AtomicOrdering::Unordered` and no alignment (default).

- [ ] **Step 3: Implement `declare_runtime_externs()` in `codegen_ir_main.h`**

Declare the `gs_*` functions that the IR code will call. The set is the same as the `GS_API` symbols exported by `libgoose_rt.a` (71 symbols per M1). For now, declare the most critical ones that the test-suite smoke check will exercise:

```cpp
void CodeGenIR::declare_runtime_externs() {
    auto i8ptr = llvm::Type::getInt8PtrTy(m.ctx);
    auto i64   = llvm::Type::getInt64Ty(m.ctx);
    auto voidty = llvm::Type::getVoidTy(m.ctx);

    // Core: abort, region management, stack init.
    declare_extern("gs_abort", voidty, {i64});
    declare_extern("gs_reserve_region", i8ptr, {i64, i64});
    declare_extern("gs_regions_init", voidty, {i64});
    declare_extern("gs_thread_spawn", i64, {i8ptr, i8ptr, i64});
    declare_extern("gs_qput", i64, {i8ptr, i8ptr, i64, i64});
    declare_extern("gs_qget", i64, {i8ptr, i8ptr, i64, i64});
    // ... add the rest as M3/M4 needs them
}
```

- [ ] **Step 4: Implement `Run()` as a function-shell stub**

For now, `Run()` creates the `llvm::Function` for each `FnSpec` (with the `i64 gs_sp` argument) and returns without walking the body. The body-walk lands in Task 3 (M3).

```cpp
void CodeGenIR::Run() {
    auto i64 = llvm::Type::getInt64Ty(m.ctx);
    auto fnty = llvm::FunctionType::get(i64, {i64}, false);
    for (auto *spec : specs) {
        auto *f = llvm::Function::Create(fnty, llvm::FunctionType::...,
                                         spec->name.c_str(), m.mod.get());
        auto *entry = llvm::BasicBlock::Create(m.ctx, "entry", f);
        b.SetInsertPoint(entry);
        // M3: walk spec->body here
        b.Ret(i64(0));
    }
}
```

- [ ] **Step 5: Add a smoke test — emit a trivial program and verify the module is valid**

Create `test/ir_smoke.goose`:
```goose
fn main {
    print("hello, ir\n");
}
```

Run (after M5 driver is in place; for now just verify the module builds):
```bash
./build/goose --emit-ir test/ir_smoke.goose -o /tmp/ir_smoke.ll
llc /tmp/ir_smoke.ll -o /tmp/ir_smoke.s
clang /tmp/ir_smoke.s -o /tmp/ir_smoke.exe -L build -lgoose_rt -lm
/tmp/ir_smoke.exe
```
Expected: `hello, ir` prints. (This is a forward reference to Task 6; the smoke test will fully pass only after Task 6.)

- [ ] **Step 6: Commit**

```bash
git add src/codegen_ir_main.h test/ir_smoke.goose
git commit -m "codegen_ir: CodeGenIR class with TLS globals and extern decls"
```

---

### Task 3: `CodeGenIR` — value emission and statement walk (M3)

**Files:**
- Modify: `src/codegen_ir_main.h` (the full body-walk: expressions, statements, in-place construction, `OpenRzDest` prefix+patch, frame objects)
- Modify: `src/codegen_ir_values.h` (promote the free-function value-emitters to `CodeGenIR` member methods; add `EmitPush`, `EmitAppend`, `EmitLoad`, `EmitStore`, `TopW`/bump, `--stack-assert` checks)

**Interfaces:**
- Consumes: `CodeGenIR::Run()` (the function shell from Task 2), `IrModuleBuilder` (the module + TLS globals), the `Ast` node types, the `FnSpec::body`
- Produces: A fully-walked IR module for a simple program (no threads, no `c_headers`, no `dylib`), whose stdout matches the C backend

**Design note:** The C codegen's `codegen_values.h` (~2k lines) and `codegen_stmts.h` (~1.2k lines) and `codegen_construct.h` (~1.4k lines) are the reference. The IR equivalents do the same AST walk but emit `IRBuilder` calls instead of C string appends. The key semantic differences:

- **`TopW` / bump:** In C, `TopW(stk)` is `stk->top` (a pointer). In IR, `stack_load(stack, 1 /* top offset */, i64)` followed by `b.CreateAdd(old_top, n)`.
- **In-place construction (`OpenRzDest`):** In C, the codegen reserves a prefix on the stack and patches it later. In IR: `alloca` (or stack-bump) → `store` → `load` → `store` (patch). The `load`/`store` pair is the IR equivalent of the C "prefix reservation + patch."
- **`--stack-assert`:** When set, every bump site inserts:
  ```cpp
  auto limit = b.CreateLoad(i64, limit_ptr, "limit");
  auto cmp = b.CreateICmpSGT(new_top, limit, "over");
  auto if_block = llvm::BasicBlock::Create(m.ctx, "overrun", f);
  auto continue_block = llvm::BasicBlock::Create(m.ctx, "bump_ok", f);
  b.CreateCondBr(cmp, if_block, continue_block);
  b.SetInsertPoint(if_block);
  auto fnty_abort = /* the already-declared @gs_abort */;
  b.CreateCall(fnty_abort, {b.SInt(i64, GS_E_BUMPOVR)});
  ```

- [ ] **Step 1: Port the expression walk from `codegen_values.h` to IR**

Walk `Ast::Expr` nodes and emit the corresponding `IRBuilder` calls. For each `NodeType`:

| C codegen (codegen_values.h) | IR equivalent |
|---|---|
| `EmitIntLit` | `b.SInt64(val)` |
| `EmitFloatLit` | `llvm::ConstantFP::get(...)` |
| `EmitBoolLit` | `b.Int1(...)` |
| `EmitStrLit` | `llvm::ConstantDataArray` + global (or `llvm::ConstantDataString`) |
| `EmitLoad` (local) | `b.CreateLoad(ty, alloca, name)` |
| `EmitStore` (local) | `b.CreateStore(val, alloca)` |
| `EmitBinOp` (`+`, `-`, `*`, ...) | `b.CreateAdd`, `b.CreateSub`, `b.CreateMul`, etc. |
| `EmitIndex` (`a[i]`) | `b.CreateInBoundsGEP(ptr, {b.Zero(i64), i}, "idx")` + load/store |
| `EmitPush` | bump + store the element at `old_top` |
| `EmitAppend` | bump + copy the element(s) |
| `EmitCall` | `b.CreateCall(fn, args, "call")` |

**Important:** Locals use `b.CreateAlloca(ty, 1, name)` at the top of the function (or on a stack that the driver manages). The data stack model (§10.3) is: every nonfixed local/temp lives on a data stack, accessed via `gs_stks[gs_sp + k]`. For the first pass, use `alloca` for fixed locals and bump the data stack for nonfixed. The full stack-assignment logic (`codegen_stacks.h`, 174 lines) is a later sub-task.

- [ ] **Step 2: Port the statement walk from `codegen_stmts.h` to IR**

`Block`, `If`, `For`, `While`, `Break`, `Continue`, `Return`, `Assert`, `Throw`, `Match`. The IR equivalents:

| C codegen | IR equivalent |
|---|---|
| `Block` | `llvm::BasicBlock` nesting |
| `If` | `b.CreateCondBr(cond, then_bb, else_bb)` |
| `For` / `While` | loop with `icmp` + `br` (or `do { } while` form) |
| `Break` / `Continue` | `b.CreateBr(merge_bb)` |
| `Return` | `b.CreateRet(val)` |
| `Assert` | `icmp` + `call @gs_abort` |

- [ ] **Step 3: Implement `OpenRzDest` (in-place construction of resizable values)**

In the C backend, `OpenRzDest` reserves a prefix on the stack (space for the `gs_rhdr` + length), and `EmitReprefix` / `EmitSlidePrefix` patch the length later. In IR:

```cpp
// Reserve space on the data stack for the frame object.
auto hdr_size = b.SInt(i64, sizeof_frame_object);
auto old_top = bump(stack, hdr_size);  // Task 3 Step 1
auto hdr_ptr = old_top;  // the frame object starts here

// Initialize the fixed fields.
b.CreateStore(field_val, b.CreateInBoundsGEP(ptr_ty, hdr_ptr, {field_idx}));

// The length is patched after the elements are built.
// (EmitReprefix: load the current top, compute len, store to hdr_ptr + len_offset)
```

- [ ] **Step 4: Add `--stack-assert` checks at bump sites**

When `CodeGenIR::stack_assert` is `true`, every bump (push/append/resize/OpenRzDest) inserts the `icmp`/`abort` pair shown in the Design note above.

- [ ] **Step 5: Port `thread_fn` entry points and `gs_thread_spawn` calls**

Each `thread_fn` specialization becomes a separate IR function (same module) with the `i64 gs_sp` argument. The `gs_thread_spawn` call passes the function pointer + argument image.

- [ ] **Step 6: Port `return E from f` (the `gs_rf` TLS discriminant)**

`gs_rf` is a TLS `i8*` (pointer to a discriminant value). At the `return ... from` site: `b.CreateStore(discriminant, gs_rf)`. At the target frame's entry: `b.CreateLoad(i8ptr, gs_rf)` and branch.

- [ ] **Step 7: Verify with a medium-complexity fixture**

Pick a fixture from `test/` that exercises push, in-place construction, and a loop. Run:
```bash
./build/goose --emit-ir test/<fixture>.goose -o /tmp/t.ll
llc /tmp/t.ll -o /tmp/t.s
clang /tmp/t.s -o /tmp/t.exe -L build -lgoose_rt -lm
/tmp/t.exe > /tmp/t.out
diff /tmp/t.out test/expected/<fixture>.out
```
Expected: no diff.

- [ ] **Step 8: Commit**

```bash
git add src/codegen_ir_main.h src/codegen_ir_values.h
git commit -m "codegen_ir: expression, statement, and in-place construction walk"
```

---

### Task 4: `CodeGenIR` — calls, `emit`, and `c_headers` adapters (M4)

**Files:**
- Modify: `src/codegen_ir_main.h` (call emission, `c_headers` adapter translation)
- Create: `src/codegen_ir_calls.h` (if the call emission is large enough to warrant a separate file; otherwise keep in `codegen_ir_main.h`)

**Interfaces:**
- Consumes: Task 3 (the function shells and value emission), the `FnSpec` call sites, the `dylib` / `c_headers` package metadata
- Produces: A fully-linked IR program that includes `extern fn` calls, `c_headers` adapters (hand-translated to IR `declare`s), and thread/queue operations

- [ ] **Step 1: Port the call emission from `codegen_calls.h` to IR**

For each call site in the AST:
- **Fixed return:** `b.CreateCall(fn, args, "r")` → the result is an `i64`/`float`/`ptr`.
- **Nonfixed return (in-place):** the destination stack is passed as a hidden argument; the call does not return a value. `b.CreateCall(fn, args + [dst_stack], "call")`.
- **`extern fn` (C interop):** the function is `declare`'d as an `ExternalLinkage` `Function` with the same signature as the C function. The name is the mangled C name (`SFunction::cname`).
- **Thread spawn:** `b.CreateCall(gs_thread_spawn_fn, {fn_ptr, arg_image, arg_size}, "tid")`.
- **Queue ops:** `b.CreateCall(gs_qput_fn, {queue_ptr, elem_ptr, elem_size, count})`.

- [ ] **Step 2: Hand-translate `c_headers` adapters to IR `declare`s (option B)**

The package's `.build` file lists `c_headers = ["include/adapters.h"]`. The adapter functions are `static inline` C functions that use the generated Goose type names (e.g. `mylib_Pair_g5`). In the IR module:

1. For each adapter function in the C header, `declare` it in the IR module as an `ExternalLinkage` `Function` with the same name and signature as the C function.
2. The `goose llc` driver (Task 6) compiles the C adapter header into a separate `.o` file and links it.

The IR module does **not** contain the adapter bodies. They are C code, compiled separately.

- [ ] **Step 3: Emit the export table for `dylib` interop**

For each `dylib` block, emit a `GlobalVariable` array that maps C symbol names to the IR functions that call them. This is the `dlbind` table the runtime uses at startup to resolve the shared library's symbols.

- [ ] **Step 4: Port queue/thread primitives from `codegen_builtins.h`**

`gs_qput`/`gs_qget` (queue ops), `gs_thread_spawn`, `gs_thread_join`. These are `declare`'d externs; the IR codegen calls them with the right arguments.

- [ ] **Step 5: Verify with a fixture that uses `extern fn` and a `dylib`**

Pick a fixture from `test/stdlib/` that calls a C function. Run the IR pipeline and verify the output.

- [ ] **Step 6: Commit**

```bash
git add src/codegen_ir_main.h src/codegen_ir_calls.h
git commit -m "codegen_ir: call emission, c_headers adapter decls, export table"
```

---

### Task 5: `goose llc` driver and `test/ir_runner.py` (M5)

**Files:**
- Modify: `src/main.cpp` (add `--emit-ir` flag; add the `goose llc` subcommand; wire `GOOSE_LLVM` to the IR emit path)
- Create: `test/ir_runner.py` (the IR fixture runner: `.ll` → `llc` → `clang` → link `libgoose_rt` → run → diff)
- Modify: `test/run_tests.py` (add `--backend {c,ir}` flag; dispatch to `ir_runner.py` when `--backend ir`)

**Interfaces:**
- Consumes: The fully-emitted IR module (Tasks 1–4), `FindGooseLLVM`'s `LLC_BIN`, `CLANG_BIN`, `LLVM_BIN_DIR`, `LLVM_LIBDIR`
- Produces: `goose llc <out>.ll` → native executable; `test/ir_runner.py` → the fixture runner for the IR path

- [ ] **Step 1: Add `--emit-ir` to `src/main.cpp`**

When `--emit-ir` is given (and `GOOSE_LLVM` is on at build time), the compiler:
1. Runs the pipeline through `bce`.
2. Constructs `CodeGenIR` and calls `Run()`.
3. Writes the module to `-o <file>.ll` (text IR).
4. Does **not** run `llc` or `clang` (that is the `goose llc` driver's job).

- [ ] **Step 2: Add the `goose llc` subcommand to `src/main.cpp`**

`goose llc <file>.ll [-o <out>.exe]`:
1. Determine the target triple from the host (`GOOSE_LLVM_BIN_DIR` or `LLVM_BIN_DIR`):
   - Linux → `x86_64-unknown-linux-gnu`
   - MSYS/Windows → `x86_64-pc-windows-msvc`
   - arm64 → `arm64-apple-macos`
2. Determine the `DataLayout` string for that triple (see the design doc §"DataLayout per platform").
3. Run `llc <file>.ll -o <file>.s -mtriple=<triple> -filetype=asm`.
4. Run `clang <file>.s -o <out>.exe -L <LLVM_LIBDIR> -lgoose_rt -lm [-L <adapter_dir>]`.
5. For packages with `c_headers`, compile the C adapter header into a `.o` and link it.

- [ ] **Step 3: Create `test/ir_runner.py`**

```python
#!/usr/bin/env python3
"""
Fixture runner for the LLVM-IR backend.

Usage: ir_runner.py <goose_binary> <fixture>.goose <expected>.out [llc] [clang]
  Exits 0 if stdout matches <expected>.out byte-for-byte.
"""
import subprocess, sys, tempfile, os

def main():
    goose_bin, fixture, expected, llc, clang = sys.argv[1:6]
    with tempfile.TemporaryDirectory() as d:
        ll_path = os.path.join(d, "prog.ll")
        s_path = os.path.join(d, "prog.s")
        exe    = os.path.join(d, "prog.exe")
        # 1. Emit IR
        subprocess.run([goose_bin, "--emit-ir", fixture, "-o", ll_path],
                       check=True, capture_output=True)
        # 2. llc -> asm
        subprocess.run([llc, ll_path, "-o", s_path,
                        "-mtriple=x86_64-unknown-linux-gnu", "-filetype=asm"],
                       check=True, capture_output=True)
        # 3. clang -> exe
        subprocess.run([clang, s_path, "-o", exe,
                        "-L", os.path.dirname(goose_bin), "-lgoose_rt", "-lm"],
                       check=True, capture_output=True)
        # 4. Run and compare
        result = subprocess.run([exe], capture_output=True, text=True)
        with open(expected) as f:
            expected_text = f.read()
        if result.stdout != expected_text:
            print(f"MISMATCH:\n--- expected ---\n{expected_text}\n--- actual ---\n{result.stdout}",
                  file=sys.stderr)
            sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Add `--backend {c,ir}` to `test/run_tests.py`**

When `--backend ir` is given:
- Skip fixtures that carry a `no-llvm` first-line marker.
- Run each remaining fixture through `ir_runner.py`.
- Compare stdout against `expected/<name>.out`.

- [ ] **Step 5: Run the IR test-suite smoke check**

```bash
cd /mnt/e/work/goose
python test/run_tests.py --backend ir --exe build/goose 2>&1 | tail -20
```
Expected: most fixtures pass; a few (threads, `c_headers`) may fail — those are M4/M7 territory.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp test/ir_runner.py test/run_tests.py
git commit -m "main: --emit-ir flag and goose llc driver; ir_runner.py"
```

---

### Task 6: Full test-suite parity (M6)

**Files:**
- Modify: `test/ir_runner.py` (add `no-llvm` marker handling, `llvm-only` marker handling, `--release` flag)
- Modify: `test/run_tests.py` (the `--backend ir` path: `--stack-assert` on by default, off when `--release`)
- Fix: any IR codegen bugs discovered by the test-suite run

**Interfaces:**
- Consumes: The full IR codegen (Tasks 1–5), the test-suite fixtures, `libgoose_rt.a`
- Produces: 100% of the non-`no-llvm` fixtures and all 31 samples pass under `--backend ir` with byte-for-byte stdout match

**This is the "divergence debug" milestone.** The IR codegen is mechanically ported from the C codegen, but subtle differences (aliasing, register pressure, TLS resolution, guard-gap interaction) will cause some fixtures to diverge. The debug loop is:

1. Run the test-suite under `--backend ir`.
2. Identify the first failing fixture.
3. Run it under both backends; diff the stdout.
4. If the diff is a value (not a crash), find the IR instruction that differs from the C backend's behavior.
5. If the diff is a crash (segfault), run the native exe under GDB; check the `gs_stks`/`gs_gl` TLS resolution.
6. Fix the IR codegen; re-run.

**Specific things to watch:**
- **TLS model:** `GeneralDynamicTLSModel` means each thread's `gs_stks` is a separate copy. If a thread fixture crashes, check that `gs_thread_spawn` passes the right `gs_sp` index into the *worker's* `gs_stks`, not the main thread's.
- **Guard-gap:** The `libgoose_rt.a`'s `gs_reserve_region` uses `mmap` + `mprotect`. The IR module does **not** re-implement this. If a fixture that grows a large array crashes with SIGSEGV, check that the `libgoose_rt.a`'s `mprotect` is not being skipped (the `PROT_NONE` tail must be in place).
- **`c_headers` adapters:** The IR module `declare`s the adapter; the `goose llc` driver compiles the C adapter and links it. If a package with `c_headers` fails to link, check that the driver is passing the right `-L` path.
- **Stack-assert:** When `--stack-assert` is on, every bump has an `icmp`/`abort`. If a fixture that does not overflow aborts, the `limit` value is wrong (check `gs_reserve_region`'s `limit` argument).

- [ ] **Step 1: Run the full test-suite under `--backend ir` and identify failures**

```bash
cd /mnt/e/work/goose
python test/run_tests.py --backend ir --exe build/goose 2>&1 | tee /tmp/ir_test_run.log
grep -c "FAIL" /tmp/ir_test_run.log
```

- [ ] **Step 2: For each failing fixture, diagnose and fix the IR codegen**

For each fixture (in order of complexity: `test/syntax/` → `test/typing/` → `test/lifetimes/` → `test/codegen/` → `test/optimizer/` → `test/runtime/` → `test/threads/` → `test/stdlib/` → `test/packages/` → `test/samples/`):

1. Run the fixture under both backends:
   ```bash
   ./build/goose --emit-ir test/<fixture>.goose -o /tmp/f.ll
   llc /tmp/f.ll -o /tmp/f.s
   clang /tmp/f.s -o /tmp/f.exe -L build -lgoose_rt -lm
   /tmp/f.exe > /tmp/f_ir.out
   # C backend:
   ./build/goose test/<fixture>.goose > /tmp/f_c.out
   diff /tmp/f_ir.out /tmp/f_c.out
   ```
2. If the diff is a crash, run under GDB:
   ```bash
   gdb /tmp/f.exe
   (gdb) run
   (gdb) bt
   ```
   Check `gs_stks`, `gs_gl` TLS resolution.
3. Fix the IR codegen (in `src/codegen_ir_main.h` or `src/codegen_ir_values.h`).
4. Re-run the fixture; verify the diff is gone.
5. Commit the fix:
   ```bash
   git add src/codegen_ir_main.h
   git commit -m "codegen_ir: fix <fixture> divergence in <pass>"
   ```

- [ ] **Step 3: Run all 31 samples under `--backend ir`**

```bash
cd /mnt/e/work/goose
python samples/run_samples.py --backend ir --exe build/goose 2>&1 | tee /tmp/ir_samples.log
grep -c "FAIL" /tmp/ir_samples.log
```

- [ ] **Step 4: Run the full test-suite with `--stack-assert` on (the default for `--backend ir`)**

```bash
cd /mnt/e/work/goose
python test/run_tests.py --backend ir --exe build/goose 2>&1 | tee /tmp/ir_test_stackassert.log
grep -c "FAIL" /tmp/ir_stackassert.log
```
Expected: 0 failures (or only the `no-llvm`-marked fixtures, which are skipped).

- [ ] **Step 5: Run the full test-suite with `--release` (stack-assert off)**

```bash
cd /mnt/e/work/goose
python test/run_tests.py --backend ir --release --exe build/goose 2>&1 | tee /tmp/ir_test_release.log
grep -c "FAIL" /tmp/ir_test_release.log
```
Expected: 0 failures.

- [ ] **Step 6: Mark the M6 milestone complete**

Add a note to `docs/design/llvm_backend.md`:
```
## Status
M6 complete: <date>. All <N> non-`no-llvm` fixtures and 31 samples
pass under `--backend ir` with `--stack-assert` on. <K> fixtures
carry `no-llvm` (list them).
```

- [ ] **Step 7: Commit**

```bash
git add test/ir_runner.py test/run_tests.py src/codegen_ir_main.h docs/design/llvm_backend.md
git commit -m "ir: full test-suite parity under --backend ir"
```

---

## Task 3 foundation findings (surveyed 2026-11-08, before starting the data-stack walk)

A read-only survey of the M2-M4 skeleton plus the `goose llc` link step
turned up three facts that change how Task 3 must be built. Recorded here so
they are not rediscovered.

### F1. The skeleton's `gs_stack` model is fictitious

`codegen_ir_stacks.h`, `codegen_ir_values.h`, `codegen_ir_frames.h` and
`codegen_ir_construct.h` all assume `gs_stack = { i8 *base, i64 top, i64 limit }`.
The real runtime struct is one field: `typedef struct { uint8_t *top; } gs_stack;`
(`src/runtime/runtime.h:908-910`). There is no `base` and no `limit`:

- The region base lives in the runtime's `gs_regions[]` registry, not the stack.
- Overrun is caught by a 1 MiB guard gap faulting, not by a `limit` compare, so
  `--stack-assert` cannot check a `limit` field that does not exist.
- Every GEP built on the 3-field struct (`ir_load_stack`, `ir_bump`,
  `ir_stack_load/store`, `IrStackLayout::of`) is wrong against the linked runtime.

Element base for a resizable value comes from its frame header
`gs_rhdr = { uint8_t *base; int64_t len; }` (`runtime_inline.h:3866`), which the
C backend keeps in the frame (`DeclStackLocal`, `codegen_stmts.h:785`). No
capacity field: growth is bumping `top` inside a 2 GiB pre-reserved region.

Consequence: the four `values.h` primitives and both `construct.h` wrappers must
be rewritten around `{ top }` + `gs_rhdr` before any array can lower. `frames.h`
type helpers (`ir_fn_type`, `ir_sp_idx`, `IrLocLayout`) are fine.

### F2. The linked runtime has no init entry point

`libgoose_rt.a` is built from `src/runtime/runtime.c` + `src/runtime/ir_stub.c`
(`CMakeLists.txt:200`). Its exported (`T`) symbols include `gs_reserve_region`,
`gs_abort`, `gs_abort_msg`, `gs_print_i64` (from the stub) - but NOT `gs_rt_init`,
`gs_regions_init`, `gs_stack_init`, or `gs_globals_init`.

- `gs_globals_init` is therefore defined by the IR module itself (done: globals
  commit `06e65211`).
- There is no `gs_rt_init` to call from `int main()`. To use data stacks, the IR
  entry must reserve each stack's region directly via the available
  `gs_reserve_region(gs_stack *)` (`runtime.h:925`), or `gs_rt_init` must be added
  to the runtime build. The current `emit.h` stub calls a nonexistent
  `gs_regions_init` and must not be used as-is.

### F3. Extern declarations in `codegen_ir_main.h` do not match `runtime.h`

Declared-but-unused externs carry wrong signatures (latent link bugs once used):
`gs_reserve_region(i64,i64)->ptr` vs real `(gs_stack*)->uint8_t*`;
`gs_thread_spawn(ptr,ptr,i64)` vs real `gs_thread_start(run,release,entry,args,argsize,contained)`;
`gs_qput/gs_qget` arity differs from `runtime.h:996-997`. Fix these against
`runtime.h` before emitting any call to them.

### Recommended Task 3 first slice (narrowest observable win)

A single growable `i64[>..]` array supporting `push`, index-read and `len`,
lowered with the CORRECT model: a per-array `gs_stack` (one `top` ptr) whose
region is reserved once in `int main()` via `gs_reserve_region`; a `gs_rhdr`
`{base,len}` frame alloca; `push` = `store i64 at top; top += 8; len++`;
`arr[i]` = bounds-checked `base[i]`; `arr.len` = read `len`. Verify against a new
`ir_minimal_array` fixture, then regress the 13 passing fixtures (they must not
break when the entry gains region-reservation). This is the prerequisite chain
for every array/ADT fixture (the ~163 current failures).
