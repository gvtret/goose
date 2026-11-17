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
int- and bool-walked locals and globals, growable arrays (`T[>..]`) with literal
and fill initializers, push/index/len with bounds checks, element
iteration, fixed arrays (`T[k]`) with literal and fill initializers,
`as` casts between int storages (checked at the value's bounds) and
into index/bound types (checked at the array's live count), direct
calls of int-signatured functions (void included), range/count loops,
while, if, break-continue, and lines print through the runtime's
format hooks one write per line (int and bool values and string literals; the
rest of print's alphabet lands with its shapes). Multi-result calls
use out-pointers for results 1.. (result 0 is the call's value), and
signed `/`, `%` follow the runtime (Euclidean `%`, checked zero and
i64.min / -1), `>>` is arithmetic (logical for an unsigned left
operand), `<<`, `&`, `|`, `^` and `~` (and their compound forms) work,
`&&`/`||` short-circuit, and a
callee's return inside an inlined body meets its call's merge in a
PHI. Index assignments (plain and compound) store into fixed and
growable int arrays with the same checks as reads, and a `continue`
lands in the loop's step block, so the counter advances exactly as a
fallthrough does. `assert` is the runtime abort on a false test, and a
global's `++` writes its global slot. `append` copies a fixed array's, a
slice's or (for a byte element) a string literal's elements onto a
growable array's top and adds their count to the length, the source
evaluated first; `pop` reads the last element's value back in value and
statement position, an empty one the runtime's `GS_E_POP` abort, and
`pop_n` drops a count of them at once past `GS_E_POPN`. A limited
array (`T[..k]` of int elements) is a fixed-slot value with a count:
a literal or a string literal (byte elements) sets it, `push` and
`append` grow it behind the declared capacity (`GS_E_CAPACITY` past
it), `pop` and `pop_n` shrink it, index reads and stores check the
count, `.len` and `.cap` read the header, a `for` names each element
and print renders the run. A
construct the walk does not emit
is refused with its kind and line (`--emit-ir` fails, no module is
written) instead of being dropped; a builtin other than `push`,
`append`, `pop`, `pop_n`, `assert`
and `print`, and `match`, `defer` or a `loop` that carries a value, count
as such constructs. A `loop` statement emits as a
body block its `break`s leave for a done block, which is what a
tail-recursive function becomes. Flat structs
(no padding, fixed size, every field an int or another such struct) are
values: a literal builds them, field reads, plain and compound field
stores work on a field path to the leaf (`a`, `a.b`, `a.b.c`) at the
leaf's own width, a copy moves the whole value between locals, and one
crosses a call by value (a struct-shaped
LLVM parameter, a single struct result, the callee's own copy in an
alloca). A reference to a flat struct crosses a call as one pointer;
a `.=` binding, a parameter, a field path root and a call result all
name the struct body through it, and a `var r = f()` binding reads the
whole value at the pointer. An int parameter gets a slot of the body's own from entry, so a
loop that reassigns it each round reads what the last round stored. A slice
(`T[:]` of int) is a `{ data, len }` view: a local is built from a
range over a fixed, growable or slice subject (`a[lo..hi]`, open and
`^k` bounds), its `.len`, element reads and writes, `for x in s`,
re-pointing, and print (raw bytes for `u8`, `[a, b, c]` otherwise) work.
A string literal is such a view of its constant bytes, and also
initializes a `u8[]` local or a `u8[>..]`'s first bytes. A slice crosses a
call by value as the `{ ptr, i64 }` it is: a parameter is copied into the
callee's own alloca (so the body may re-point it), and a call's single
slice result returns the struct itself; a multi-result signature with a
slice still refuses. A reference to an int (`T&`) is the C backend's
pointer: it crosses a call as one (`i8*`, the argument the variable's
slot), a parameter's holder is the argument, a `let px .= n` or
`var r .= n` binding stores the slot of the variable or the struct
field its initializer names (an `&` is what the walk reads through,
whether the checker's synthetic one or the source's own), a
read loads the pointee at its own width, a plain assignment and `+=`
write it, `++` steps it, and a rebind (`r .= m`) stores the new slot
into the holder. A function may return such a reference (`-> i64&`): the
shell's result is the pointer itself, the returning expression's address
(a local's slot, a global's, a reference parameter's pointee, a field's
leaf) is what it hands back, and a value position reads through it while
`var r .= f()` keeps the pointer, so the binding writes the original.
A reference to a growable array (`T[>..]&`) is the pair
the C backend carries, `{ header, stack }` as two pointers: the local or
global header is one `{ base, count }` record (the C `gs_rhdr`), a
reference binding stores the pair in its holder and every use loads it
again, so a rebind is seen; `push` through the reference lands on the
array's top and count, and such a parameter crosses a call as two
arguments (the walk's argument index advances by what each parameter
consumed). `for &x in xs` binds the element's address, so the body's
writes land in the array. An int local of a narrow storage type
(`u32`, `u16`, `u8` and their signed forms) truncates on every store to
it, and a read widens by its signedness; `/` and `%` on an unsigned
operand are `udiv`/`urem` (a zero divisor still aborts), and `u64`
prints through the unsigned hook, as the C backend's does. Every local's
storage is allocated in the function's entry block, so a declaration in
a loop body reuses its slot instead of walking the native stack off its
end. A call may hand a growable array back (the run form): the callee
receives the caller's header and stack as its last two arguments and
copies the returned run there, writing the count; the result then binds
to a `let`, prints, indexes, measures its `.len`, and appends from, and
a `let s = f()` with an inlined `f` builds it the same way. The return
may be a value built at the site: an empty or listed literal is written
into the destination element by element, a string literal's bytes are
copied (byte elements), and a slice view is copied over. A string or
array literal in a view position resolves as its bytes or elements
whatever array type the optimizer folded it to. `move_from` on int-array
views checks lengths match (aborting with the runtime's message when
they do not) and memmoves the source over the destination, so
overlapping slices like `a[1..].move_from(a[..4])` shift correctly.
`clear()` on a growable array drops its count to zero and its stack top
back to the element region's base; on a static-capacity limited array it
zeroes only the count. An optional reference (`T&?`, int or flat struct)
reads as its pointer (null when unset) for `== null`, `!= null` and bare
truthiness; `null`, `default<T&?>()` and rebind via `ref_addr` (locals,
structs, globals, another reference) set the holder. A float value lives
at its own width: literals, locals, `+ - * /` and their compound forms
run in float arithmetic, print renders through the runtime's float
hooks, and an f32 widens where it mixes with an f64. A growable array
of float elements (`f64[>..]`, `f32[>..]`) works like one of ints:
literals, push, index, `.len`, a `for` binder at its own width, and
print through the float hooks. A flat struct may hold float fields at
their own width: literals narrow, reads load as floats, stores and
compound forms run in float arithmetic, and equality compares floats
(float pairs with FCmp, a struct field bit-exact). A flat struct may
also hold a slice field (`const u8[:]`, `i32[:]`): its { data, len }
pair lands in the struct body, a literal builds it from a string
literal or a view, field paths read it as a view (print and .len work
through them), a copy moves the whole value, and a plain assignment
re-points it. `default<T>()` builds the type's zero: 0, +0.0, the null
pointer of an optional reference, the null pair of a slice, and for a
flat struct the folded literal whose given fields build and whose rest
keep their zero. print renders a struct through the default hook: the
type's name, `{ field, field }`, string fields in quotes, nested
structs recursively, and float fields through the float hooks; an array
of flat structs prints `[T { ... }, T { ... }]`, and a multi-result call
binds result 0 from the call's value and results 1.. through
out-pointers. A fixed array of flat structs builds its literal in place:
a field path reaches through an element (`pair[1].name`), a `for` binds
each element as a struct, and print renders the whole run. A fixed array crosses a
call by value (the C backend's `a2_i64`): a parameter copies into the
body's own storage, a result returns the whole array (an inlined
callee's parameter binding copies the named run), and a fixed local
reads as its array value where a value is wanted. A reference may bind
an array element's storage (`let r .= a[i]`): its address is the element's
slot, so a write through the reference reaches the array. `str(a, b, ...)`
renders values into a fresh growable buffer and returns its view;
`format(out, a, ...)` renders into an existing growable. Both share print's
renderer, write no newline, and refuse a user format hook (a later cut).
An `as` cast changes a value's width: an int widens to a float, a float
changes width, and `as!` narrows a float to an int (truncating toward zero).
A zero-length fixed array (`i64[0]`) is a `[0 x T]` value with no storage:
it crosses a call and reads its `.len` as zero.
An append's source may be a run built at
the site: a growable-returning call, and an if of them, builds onto a
scratch destination before its elements join the receiver. `copy(x)`
makes a fresh copy of an array value (a growable onto a scratch run, a
fixed array into a temporary), and a growable global's literal
initializer lands on its region when the program starts. A fixed
int-array global holds its elements in thread-local storage: its literal
initializer (listed or a string literal's bytes) lands there in
`gs_globals_init`, and print, index, `.len` and `copy` see the run. An `extern fn`'
call lowers straight to its C symbol: floats at their width, ints as
i64, an int or float reference as a pointer (a float local's own
storage), an int-array slice as the { data, len } pair; float
comparisons (`<`, `>`, `<=`, `>=` and equality) run as FCmp on the
operands' float type. An optional reference to a growable array
(`u8[>..]?`) holds the pair of two null pointers when null, its
expression value reads as the header pointer (so `== null`, `!= null`
and a bare `if o` test it), and `.=` binds it like a plain reference.
`goose llc`
passes `-relocation-model=pic`, so a
private constant's address links into the host's PIE executable. Fixtures
that passed only because their unemitted statements or locals did not show
in the output now refuse. 68 of the codegen output fixtures pass through
`--emit-ir`; the largest remaining gaps are references to other
aggregates (fixed arrays, structs, enums),
limited arrays
built by copy from other values (the runtime-capacity `T[..]` form with
them), other call results (a length-stored `T[]`, a runtime-limited
`T[..]` without a capacity, literals and slices returned as arrays),
struct parameters and copies,
struct fields of slices or
references, and rendering of floats and structs,
plus the M4 modules. See
`docs/superpowers/specs/2026-11-07-llvm-backend-design.md` for the
full design and partner decisions.
