// LLVM-IR backend: umbrella header.
// Includes the IR codegen pieces in dependency order.
// #include from main.cpp when GOOSE_LLVM is on.
#pragma once
#include "codegen_ir_main.h"
#include "codegen_ir_values.h"
#include "codegen_ir_frames.h"
#include "codegen_ir_stacks.h"
#include "codegen_ir_construct.h"
#include "codegen_ir_calls.h"
#include "codegen_ir_emit.h"
