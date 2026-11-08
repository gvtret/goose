# The LLVM-IR backend

The LLVM-IR backend emits LLVM IR instead of C. It is selected at build
time with `GOOSE_LLVM=on` (default `off` - the C backend remains the
default, see [implementation section 8](../implementation.md#8-the-llvm-backend));
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

- `GOOSE_LLVM=on` - builds the IR backend into the compiler. Adds a
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

- `cmake/FindGooseLLVM.cmake` - finds `llvm-config`, locates
  `LLVM_INCLUDE_DIRS`, `LLVM_LIBDIR`, `LLC_BIN`, `CLANG_BIN`; builds
  `libgoose_rt.a` via a custom command; sets `GooseLLVM_FOUND`.
- `src/runtime/runtime.c` - the five-header runtime bundle with
  external linkage (`GS_SEPARATE_RUNTIME=1`, `GS_RUNTIME_OBJECT=1`,
  `GS_NEED_THREADS=1`); TLS state stays in the program TU, the archive
  exports only `GS_API` symbols.
- Root `CMakeLists.txt` - `GOOSE_LLVM` option; when on, builds the
  runtime target and links LLVM.

## M2: type + value emission (skeleton)

- `src/llvm/codegen_ir_frames.h` - layout helpers (`IrStackLayout`,
  `IrLocLayout`, `ir_fn_type`, `ir_sp_idx`); alignment mirrors the C
  backend's `#pragma pack(1)` convention. The planned
  `codegen_ir_types.h` was folded into this header.
- `src/llvm/codegen_ir_values.h` - `ir_load_stack`, `ir_bump`,
  `ir_stack_load`/`ir_stack_store`. Built against the runtime's real
  one-field `gs_stack`; see "Task 3 foundation findings" in
  `docs/superpowers/plans/2026-11-07-llvm-ir-codegen.md`.

## M3: frames, stacks, construction (partially done)

- `src/llvm/codegen_ir_frames.h` - `gs_sp` (i64 index into `@gs_stks`),
  `gs_at` (i8* to `gs_loc`), TLS globals.
- `src/llvm/codegen_ir_stacks.h` - stack-block constexprs. The data
  stack is the runtime's one-field `gs_stack = { top }`; region bases
  live in the runtime's `gs_regions[]` registry (the `{ base, top,
  limit }` layout named here earlier was fictitious).
- `src/llvm/codegen_ir_construct.h` - in-place construction, `OpenRzDest`,
  frame objects.
- Minimal body-walk (`src/llvm/codegen_ir_main.h`): `IntLit`, `Binary`
  (+,-,*,/),
  `Block`, `Call` (print via `gs_print_i64`, direct i64 calls),
  `VarDecl`/`Assign`/`IncDec`/`Return` for i64 locals, plus the native
  `int main(argc, argv)` entry - enough for 9 `ir_minimal_*` fixtures
  to pass end-to-end. Growable `i64[>..]` values lower on one real
  data stack per array: a lazy thread-local `gs_stack { top }` slot
  reserved in `main` through `gs_rt_start` plus `gs_reserve_region`,
  a `gs_rhdr { base, len }` header, `push`/`a[i]`/`a.len` (the first
  `ir_minimal_array` fixture). The full walk (resizable values of all
  shapes, in-place construction, calls, value-carrying if/EarlyBlock
  expressions, `return E from f`, thread thunks) is the remainder of
  M3.

## M4: calls, emit, c_headers adapters

- `src/llvm/codegen_ir_calls.h` - extern calls, thread spawn, queue ops.
- `src/llvm/codegen_ir_emit.h` - module layout, export table, `dlbind`.
- **c_headers adapters** (partner decision: option B - hand-translated
  IR declarations, not Clang-compiled objects). The IR module
  `declare`s each adapter; the `goose llc` driver compiles the C
  adapter into a separate object and links it.

## M5: `goose llc` driver (done)

- Target triple + `DataLayout` selection per platform.
- Links `libgoose_rt.a` + adapter objects.
- `test/ir_runner.py` - fixture runner for the IR path.
- `src/main.cpp` - `goose llc` subcommand (llc -> clang -> exe).

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

The IR backend emits honest programs for the M3 data-stack subset:
int-walked locals and globals, growable arrays (`T[>..]`) with literal
and fill initializers, push/index/len with bounds checks, element
iteration, fixed arrays (`T[k]`) with literal and fill initializers,
`as` casts between int storages (checked at the value's bounds) and
into index/bound types (checked at the array's live count), direct
calls of int-signatured functions (void included), range/count loops,
while, if, break-continue, and lines print through the runtime's
format hooks one write per line (int values and string literals; the
rest of print's alphabet lands with its shapes). Multi-result calls
use out-pointers for results 1.. (result 0 is the call's value), and
signed `/`, `%` follow the runtime (Euclidean `%`, checked zero and
i64.min / -1), `>>` is arithmetic, `&&`/`||` short-circuit, and a
callee's return inside an inlined body meets its call's merge in a
PHI. 32 of the codegen output fixtures pass through `--emit-ir`; the
rest need the remaining M3 shapes (structures, arrays returned and
sliced, strings as values) and the M4 modules. See
`docs/superpowers/specs/2026-11-07-llvm-backend-design.md` for the
full design and partner decisions.
