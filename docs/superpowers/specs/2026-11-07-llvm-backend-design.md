# LLVM-IR Backend for Goose — Design

## Summary

Add a new codegen backend for Goose that emits LLVM IR instead of C.
The backend runs in parallel to the existing C backend, selected at build time
via a CMake flag `GOOSE_LLVM=on` (default `off` — keep the C backend as the
default for zero-impact rollout).

The IR backend is a drop-in replacement for `codegen*.h` at the pipeline
level: same parser, same typecheck, same optimizer, same BCE, same dump,
same LSP, same test fixture format. It changes only:

- `src/codegen*.h` (~11k lines) → new `src/codegen_ir*.h` that emit IR
  through LLVM's `IRBuilder`. Not a rename: C codegen's `string body`
  buffer plus C spellings (`ctypes`, `Mangle`, `TopW`, `SpIdx`,
  `CacheableStk`, `SyncReach`) are fully replaced with
  `llvm::Type*` / `llvm::Value*` / `llvm::StructType*` mechanics.
  See §"Where it lives" below for the per-module mapping.
- `src/runtime/runtime_inline.h` + `src/runtime/*.h` (6.6k + 5.7k) →
  kept as C, built into `libgoose_rt.a` with Clang. The emitted IR
  program links against this archive; IR codegen only calls into
  `gs_*` symbols, it does not re-implement the runtime.
- `cmake/libtcc.cmake` (TinyCC) — unchanged in M1–M5. TinyCC is
  always built (no CMake flag for it; see `CMakeLists.txt:147`). A
  separate, opt-in CMake flag `GOOSE_ORC_JIT=on` adds ORC-JIT as a
  JIT alternative to TinyCC. TinyCC removal is out of scope.
- C interop (`typecheck_calls.h` + `codegen_emit.h`): the C backend
  is untouched. The IR backend's emit layer rewrites `gs_*` symbol
  exports to match LLVM module layout (same symbol names, different
  linkage). See §"c_headers adapters in IR" for the treatment of
  inline C adapters (hand-translation to IR declarations, per partner
  decision 1: option B).
- Fixtures: identical. Test suite runs against both backends and
  compares output against `expected/<name>.out`. For `--backend ir`
  the test harness compiles `.ll` → `llc` → `clang` → native exe,
  links `libgoose_rt.a`, runs it, and diffs stdout. A `no-llvm`
  fixture marker skips IR compilation for a specific fixture. See
  §"Test strategy" below.

## Goals

1. **No change to parser, typecheck, optimizer, BCE, dump, LSP, format, or
   stdlib semantics.** The IR backend is a new leaf in the pipeline.
2. **Same test suite passes.** Every fixture in `test/` and every sample in
   `samples/` compiles to IR, runs (natively via `llc + clang`, or via ORC
   JIT), and its stdout matches `expected/<name>.out`.
3. **C interop still works.** `extern fn` and `c_headers` in `.build`
   files continue to work; the only change is that adapters are IR
   declarations instead of C prototypes.
4. **JIT with threads.** ORC JIT (when `GOOSE_ORC_JIT=on`) removes the
   TinyCC `SHF_TLS` limitation; `main.cpp:1772-1779` diagnostic becomes a
   no-op under ORC.
5. **Wasm path unblocked.** Later target triple (`wasm32-unknown-wasi`)
   uses the same IR builder as native x86-64.

## Non-goals

- Replacing the C backend. C backend stays the default; `GOOSE_LLVM=on`
  is opt-in.
- Web UI or documentation site.
- Rewriting the standard library. Stdlib is Goose source and gets compiled
  by either backend.
- Cross-compiling to Wasm in this pass. Only `x86_64` (MSYS/Windows),
  `x86_64-unknown-linux-gnu`, `arm64` (macOS later).
- ORC JIT in the first build; ORC JIT lands as a follow-up. Native
  `llc` + `clang` is the initial target.

## How it works

### Pipeline shape

```
parse ─▶ dump ─▶ typecheck (monomorphize) ─▶ optimize ─▶ bce
                                                              │
                                                            ▼
                                               ┌─ codegen_c ─▶ C file ─▶ external C compiler
                                               └─ codegen_ir ─▶ LLVM IR ─▶ llc + clang  (or ORC JIT)
```

Both codegen variants consume the same AST + `FnSpec` specializations from
typecheck. They differ only in their output.

### Where it lives

```
src/codegen_ir.h                # IRBuilder-based CodeGen; replaces C codegen's
                                #   string buffer + C spellings with llvm::Type*
                                #   / llvm::Value* / llvm::StructType*
src/codegen_ir_types.h          # layout, StructType, DataLayout, alignment
src/codegen_ir_values.h         # EmitPush, EmitAppend, EmitLoad, EmitStore
src/codegen_ir_calls.h          # extern calls, thread spawn, queue ops,
                                #   hand-translated c_headers adapters (IR decls)
src/codegen_ir_emit.h           # module layout, export table, dlbind
src/codegen_ir_frames.h         # stack frames, gs_sp / gs_at conventions
src/codegen_ir_construct.h      # in-place construction, OpenRzDest prefix+patch
src/codegen_ir_stacks.h         # data stack layout, TopW / bump

src/runtime/*.h                # unchanged; compiled with Clang into
                                #   libgoose_rt.a (external linkage)
src/runtime/ir_glue.c         # NEW: small glue that flips runtime's internal
src/runtime/ir_glue.c           # static GS_API declarations to external linkage,
                                #   so libgoose_rt.a exports the symbols the
                                #   IR module references. No logic changes.
src/main.cpp                   # new flag GOOSE_LLVM=on -> IR emit path
cmake/FindGooseLLVM.cmake      # NEW: wraps llvm-config; exposes LLVM_DIR,
cmake/FindGooseLLVM.cmake       #   LLVM_LIBS, LLVM_BIN_DIR (for llc/clang);
                                #   not a FindLLVM replacement, just glue that
                                #   knows Goose's layout
```

### Module layout of an emitted IR program

For a program with `thread_fn` and `extern "SDL2"`, the emitted module
contains:

1. One `Function` per `FnSpec` specialization. Function name is the same
   mangled name the C backend uses for **functions** (e.g. `f`, `g`, `main`)
   — *not* `mylib_Pair_g5` (which is the mangled C **type** name, used in
   adapters, see §"c_headers adapters in IR"). Types in the IR are
   `StructType`s (named or anonymous), and the export table (for `dylib`
   interop) maps C symbol names to IR functions.
2. Six thread-local globals (LLVM `thread_local` addressing,
   `GeneralDynamicTLSModel`):
   - `@gs_gl: i8*` (per-thread globals region pointer),
   - `@gs_stks: i8**` (array of `gs_stack*`, one per thread program),
   - `@gs_nstks: i64` (count of stack regions),
   - `@gs_current_thread_id: i64`,
   - `@gs_regions: i8**` (registry of reserved address ranges),
   - `@gs_regions_cap: i64` (capacity of that registry).
3. Extern declarations for the `gs_*` runtime functions (from
   Clang-built `libgoose_rt.a`), with `external` linkage. The IR code
   only **calls** these; it does not re-implement any runtime logic.
4. For `dylib` blocks: `external` external declarations whose names
   match the C symbol the shared library exports. These are
   `declare`d, not `define`d.
5. For `c_headers` adapters: hand-translated IR `define`'d functions
   that carry the same semantics as the C adapters (see §"c_headers
   adapters in IR").

### Calling convention

Identical to the C backend, but expressed in LLVM terms:

- Every function takes an extra **`i64` `gs_sp` argument** (an *index*
  into the `@gs_stks` array, not a pointer; cf. `codegen_frames.h:623`
  where the C backend emits `int64_t gs_sp`). The function reads
  `gs_stack* stk = load i8**, @gs_stks; load i8*, @gs_stks + (gs_sp * 8)`
  to get its data stack, then uses `TopW(stk)` / bump to push.
- `gs_at` is a **`i8*` (pointer to `gs_loc`)** argument, passed
  explicitly as in C. No register-convention trickery; LLVM decides
  register allocation.
- TLS access is via LLVM `thread_local` globals. The emitted module
  uses `GeneralDynamicTLSModel` (partner decision 3: GeneralDynamic
  everywhere, no per-platform InitialExec).

### c_headers adapters in IR (partner decision 1: option B)

The C backend supports `c_headers = ["include/adapters.h"]` in a
package's `.build` file. The C header is `#include`d into the
generated `.c` file and may contain `static inline` adapters that use
the generated Goose **type names** (e.g. `mylib_Pair_g5`).

In the IR backend, per partner decision, these adapters are
**hand-translated to IR declarations** — not Clang-compiled into an
object. Concretely:

- The generated IR module `declare`s each adapter as an `external`
  `Function` (no body in the IR module).
- The `goose llc` driver emits a small **support C file** that
  defines these adapter functions as `extern` (the actual C source is
  the package's `c_headers`, but compiled separately and linked in).
- The IR module's `@<adapter>_g` symbols resolve to the link-time
  C symbols.

This keeps the four external packages (gfx, audio, ui, physics)
**unchanged in C** for this pass; the IR backend just declares their
adapters and relies on the linker to find the C object. The trade-off
is that `--emit-ir` alone (without link) is not directly executable;
it must go through `goose llc` (M5) which adds the C adapter objects.

### Stack layout of data stacks

Same as the C backend: each stack is a reserved address range with a
guard gap. The IR backend expresses this via:

- `@gs_regions: [N x i8*]` registry (TLS global, per thread program).
- Each stack index `i` references `gs_regions[i]` which holds the
  `{ i8* base, i64 top, i64 limit }` struct (the C `gs_stack` layout).
- Bump is `top + n` stored to `top`; **no capacity check in native
  mode** — the guard gap (a 1 MiB unmapped tail, `runtime_inline.h:145`)
  catches overruns via a fault handler. The guard-gap *setup* is
  inside `libgoose_rt.a` (Clang-compiled C, `gs_reserve_region`);
  the IR code does **not** re-implement it.
- **Partner decision 2 (runtime asserts):** for debug builds, add a
  `--stack-assert` mode that inserts explicit `if (top + n > limit)
  abort()` checks at every bump site (push/append/resize/OpenRzDest).
  This is an opt-in flag, on by default in test-suite runs, off in
  release. The assertions are generated by `codegen_ir_values.h` at
  the same places the C backend would emit `assert(...)` — the IR
  builder inserts the `icmp`/`call` pair.

### What stays the same

- AST, `FnSpec`, all analyses (parse, typecheck, optimize, BCE, LSP,
  dump, format, build).
- `expected/<name>.out` test files.
- `// error:` / `// warning:` markers.
- stdlib is Goose source, compiled by either backend.
- `thread_fn` is a **separate function in the same module** (not a
  separate module); it is called via `gs_thread_spawn` and receives
  flat-copied arguments.
- `return E from f`: the discriminant travels in the TLS `@gs_rf`
  global (C: `codegen_frames.h:17`); the IR backend writes to
  `@gs_rf` at the `return ... from` site and the callee checks it
  after returning.
- **BCE runs before IR codegen.** IR codegen sees the BCE-reduced
  AST and emits exactly the checks BCE says to keep. The IR builder
  does **not** re-do BCE; it trusts the AST.

## Guard-gap and TLS model

- **Guard-gap:** implemented in `libgoose_rt.a` (C, Clang-compiled).
  The IR module only calls `gs_reserve_region`, `gs_regions_init`,
  etc. The IR builder does **not** emit `mmap`/`mprotect`.
- **TLS model:** `GeneralDynamicTLSModel` (partner decision 3). For
  all three platforms (x86_64-linux, x86_64-msys, arm64-mac). This
  is the LLVM default for dynamic libraries; ORC JIT (M7) will use
  the same model. No per-platform InitialExec optimization in this
  pass.

## DataLayout per platform

The emitted IR module carries a `DataLayout` string. The `goose llc`
driver picks the target triple from the host (MSYS →
`x86_64-pc-windows-msvc`, Linux → `x86_64-unknown-linux-gnu`,
arm64 → `arm64-apple-macos`). The DataLayout strings are:

- `x86_64-unknown-linux-gnu`:
  `e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128`
- `x86_64-pc-windows-msvc`:
  `e-m:x-i64:64-f80:128-n8:16:32:64-S128:256`
- `arm64-apple-macos`:
  `e-m:o-i64:64-i128:128-n32:64-S128`

These are set by the `goose llc` driver (M5), not by the IR builder.
The IR builder assumes a generic `DataLayout` and lets the driver
stamp it.

## Test strategy

- Add `test/run_tests.py --backend {c,ir}` flag (default `c`).
  `--backend ir` runs:
  1. `goose --emit-ir --stack-assert <fixture>.goose -o <out>.ll`
     (produces text IR with debug assertions enabled).
  2. `llc <out>.ll -o <out>.s` (target triple from host).
  3. `clang <out>.s -o <out>.exe -L<libgoose_rt> -lgoose_rt -lm`
     (links the runtime archive).
  4. Run `<out>.exe` and compare stdout with `expected/<name>.out`.
- New fixture markers:
  - `no-llvm` (first line) — skip IR backend for this fixture.
  - `llvm-only` (first line) — run **only** the IR backend (for
    ORC-JIT-specific thread tests, M7).
- New runner file `test/ir_runner.py` (or extension of
  `native_builder.py`): owns steps 1–4 above. `run_tests.py`
  dispatches to it when `--backend ir`.
- `--stack-assert` is on by default for `--backend ir` runs;
  `run_tests.py` passes it unless `--release` is given.

## Build strategy

- `cmake/FindGooseLLVM.cmake` (new, wraps `llvm-config`):
  - `find_package(LLVM QUIET)` to get `LLVM_DIR`, `LLVM_LIBS`,
    `LLVM_INCLUDE_DIRS`.
  - `find_program(LLC_BIN llc HINTS ${LLVM_BIN_DIR})`.
  - `find_program(CLANG_BIN clang HINTS ${LLVM_BIN_DIR})`.
  - Builds `libgoose_rt.a`: Clang-compiles each `src/runtime/*.h`
    into an object with `GS_API = extern` (external linkage) plus
    `src/runtime/ir_glue.c` (a small glue that flips the runtime's
    internal `static` declarations to external so the IR module can
    link against them).
  - `goose` executable: adds `-lLLVM-<ver>` (or `libLLVM-*.so` on
    Windows) for the IR builder; `-lLLVMORC` only when
    `GOOSE_ORC_JIT=on`.
- LLVM minimum version: **15** (stable `orc::LLJIT`, `thread_local`
  addressing, `DataLayout` for all three targets). `FindGooseLLVM`
  fails with a clear diagnostic if < 15.

## Milestones (revised after builder-check)

| # | Deliverable | Estimate |
|---|---|---|
| M1 | `FindGooseLLVM.cmake`, `libgoose_rt.a` (Clang-built, external linkage), `ir_glue.c`, CMake wiring for `GOOSE_LLVM=on` | 2 weeks |
| M2 | `codegen_ir_types.h` (layouts, StructType, DataLayout) + `codegen_ir_values.h` (EmitPush/Append/Load/Store) | 3 weeks |
| M3 | `codegen_ir_frames.h`, `codegen_ir_stacks.h`, `codegen_ir_construct.h` (incl. OpenRzDest prefix+patch, in-place construction) | 3 weeks |
| M4 | `codegen_ir_calls.h` (extern, thread spawn, queue ops), `codegen_ir_emit.h`, **hand-translated c_headers adapters** (partner decision 1: option B) | 4 weeks |
| M5 | `main.cpp` IR emit + `goose llc` driver (target triple + DataLayout + link `libgoose_rt`) + `test/ir_runner.py` + IR smoke tests | 2 weeks |
| M6 | Full test-suite parity on IR (~100 fixtures, 31 samples) with **runtime assert** checks on (partner decision 2); diff stdout byte-for-byte against `expected/` | 5 weeks (3–5 weeks debug divergence) |
| M7 | ORC JIT (`GOOSE_ORC_JIT=on`), GeneralDynamic TLS (partner decision 3), removes TinyCC SHF_TLS limitation | 5–6 weeks |
| M8 | Documentation (`docs/implementation.md` §8, `docs/design/llvm_backend.md`), `--emit-ir` flag, `--emit-bitcode` opt-in | 1 week |

**Total: 25–30 weeks for one engineer** (revised up from 11–13).
With two engineers working in parallel on M2/M3 and M4/M5, the
critical path is M6 (divergence debug) + M7 (ORC TLS), so **18–22
weeks** is the realistic two-person estimate. ORC + c_headers
adapters are the hardest parts and need one person who knows both
LLVM and the Goose runtime.

## Open questions — resolved

- **Q1.** Text IR (`.ll`) for the first release; bitcode as
  `--emit-bitcode` opt-in (M8).
- **Q2.** TinyCC stays the default JIT; ORC JIT is opt-in
  (`GOOSE_ORC_JIT=on`). TinyCC removal is out of scope.
- **Q3.** LLVM 15 floor. `FindGooseLLVM` fails with a diagnostic
  if < 15.
- **Q4.** `--emit-ir` flag yes; `--emit-bitcode` no (follow-up).
- **Q5 (new, partner decision 1).** c_headers adapters: hand-translated
  IR declarations (option B), not Clang-compiled objects. The four
  external packages stay in C; the IR module declares their adapters
  and the linker resolves them.
- **Q6 (new, partner decision 2).** Runtime assert: `--stack-assert`
  flag inserts `if (top+n > limit) abort()` at every bump site.
  On by default in test-suite runs, off in release.
- **Q7 (new, partner decision 3).** TLS model: `GeneralDynamic`
  everywhere (no per-platform InitialExec).
