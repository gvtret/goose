# Audit ledger - plan: auditor goose compiler (full codebase, 70k lines)

Context: read-only audit of src/*.h, src/main.cpp, stdlib, jit, native.
No worktree: the audit produces only a report file and an issue template, with no changes to the code.
Ruling: since the codebase is read-only, we won't create a git worktree; the audit artifacts are in .superpowers/sdd/.

Tasks:
 1. Frontend: lexer.h, parser.h, resolve.h
 2. Type check: typecheck*.h
 3. Code generation: codegen*.h
 4. Optimizer and analysis: optimize*.h, bce.h, type_queries.h
 5. JIT/native: jit.h, jit_runtime.c, native.h
 6. LSP and utilities: lsp*.h, json.h, utils.h, format.h, includes.h, main.cpp
 7. standard library: stdlib/**/*.goose
 8. build files: build.h, buildfile.h, packages.h, toolchain.h

Status: in progress
