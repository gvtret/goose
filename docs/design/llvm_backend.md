# The LLVM-IR backend

The LLVM-IR backend emits LLVM IR instead of C. It is selected at build
time with `GOOSE_LLVM=on` (default `off` — the C backend remains the
default, see [implementation §8](../implementation.md#8-the-llvm-backend));
the two backends share the pipeline up to `bce` and diverge only in
their output.

```
parse -> dump -> typecheck -> optimize -> bce
                                           |
                                       +---+---+
                                       |       |
                                   codegen_c  codegen_ir
                                       |       |
                                       v       v
                                       C   LLVM IR
                                       |       |
                              external  +---+---+
                              C compiler|   |
                                      v   v
                              native exe  llc + clang (native exe)
                                         or ORC JIT (M7)
```

## Build configuration

- `GOOSE_LLVM=on` — builds the IR backend into the compiler. Adds a
  static `libgoose_rt.a` (the runtime with external linkage) and links
  `LLVM` into the `goose` binary so that `IRBuilder` calls are available.
- The emitted IR links against `libgoose_rt.a`; the C runtime (mmap,
  threads, fault handling, TLS state) stays in the archive, and the IR
  code only calls into `gs_*` symbols.
- The emitted IR module is `declare`'d against the `gs_*` runtime
  functions; no `define`'d runtime code appears in the IR module itself.

## Minimum LLVM version

LLVM 15 (stable `orc::LLJIT` for M7, `thread_local` addressing,
`DataLayout` for all three target platforms). The `FindGooseLLVM` CMake
module reports a clear diagnostic if the system LLVM is older.

## M1: infrastructure (done)

- `cmake/FindGooseLLVM.cmake` — finds `llvm-config`, locates
  `LLVM_INCLUDE_DIRS`, `LLVM_LIBDIR`, `LLC_BIN`, `CLANG_BIN`; builds
  `libgoose_rt.a` via a custom command; sets `GooseLLVM_FOUND`.
- `src/runtime/runtime.c` — the five-header runtime bundle with
  external linkage (`GS_SEPARATE_RUNTIME=1`, `GS_RUNTIME_OBJECT=1`,
  `GS_NEED_THREADS=1`); TLS state stays in the program TU, the archive
  exports only `GS_API` symbols.
- Root `CMakeLists.txt` — `GOOSE_LLVM` option; when on, builds the
  runtime target and links LLVM.

## M2: type + value emission (in progress)

- `src/codegen_ir_types.h` — layout, `StructType`, `DataLayout`,
  alignment (mirrors the C backend's `#pragma pack(1)` convention).
- `src/codegen_ir_values.h` — `EmitPush`, `EmitAppend`, `EmitLoad`,
  `EmitStore`, `TopW` / bump mechanics, `OpenRzDest` prefix+patch.

## M3: frames, stacks, construction

- `src/codegen_ir_frames.h` — `gs_sp` (i64 index into `@gs_stks`),
  `gs_at` (i8* to `gs_loc`), TLS globals.
- `src/codegen_ir_stacks.h` — data stack layout (`{ i8* base, i64 top,
  i64 limit }`), region registry, guard-gap notes.
- `src/codegen_ir_construct.h` — in-place construction, `OpenRzDest`,
  frame objects.

## M4: calls, emit, c_headers adapters

- `src/codegen_ir_calls.h` — extern calls, thread spawn, queue ops.
- `src/codegen_ir_emit.h` — module layout, export table, `dlbind`.
- **c_headers adapters** (partner decision: option B — hand-translated
  IR declarations, not Clang-compiled objects). The IR module
  `declare`s each adapter; the `goose llc` driver compiles the C
  adapter into a separate object and links it.

## M5: `goose llc` driver

- Target triple + `DataLayout` selection per platform.
- Links `libgoose_rt.a` + adapter objects.
- `test/ir_runner.py` — fixture runner for the IR path.

## M6: test-suite parity

- All fixtures and samples compile through the IR backend and their
  stdout matches `expected/<name>.out` byte-for-byte.
- Runtime assert checks (`--stack-assert`) enabled by default in
  test-suite runs (partner decision 2).

## M7: ORC JIT

- `GOOSE_ORC_JIT=on` CMake option; `GeneralDynamicTLSModel`
  (partner decision 3). Removes the TinyCC `SHF_TLS` limitation.

## M8: documentation

- This file. `--emit-ir` and `--emit-bitcode` CLI flags.

## Status

See `docs/superpowers/specs/2026-11-07-llvm-backend-design.md` for the
full design and partner decisions.
