// LLVM-IR backend: in-place construction helpers.
//
// Empty for now: the earlier ir_open_rz_dest / ir_patch_prefix sketch
// rode the withdrawn `{ base, top, limit }` gs_stack bump (see
// codegen_ir_values.h). The C backend's OpenRzDest prefix mechanics
// (codegen_builtins.h:1391, codegen_calls.h:409) come back as the full
// M3/RzDest machinery against real regions and gs_rhdr { base, len }.
#pragma once
#include "codegen_ir_main.h"
