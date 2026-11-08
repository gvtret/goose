// LLVM-IR backend: value emission primitives.
//
// Empty for now: the earlier sketch here lowered a fictitious
// `{ base, top, limit }` gs_stack (runtime.c has `{ top }` only, the
// region base living in the gs_regions[] registry), so the primitives
// are withdrawn until the full M3 machinery lowers them against the
// real contract (runtime.h:900-910, runtime_inline.h gs_rhdr). The
// minimal body walk (codegen_ir_main.h) carries the data-stack slice
// on its own emit_growable_decl / B_PUSH / Index / Dot paths in the
// meantime.
#pragma once
#include "codegen_ir_main.h"
