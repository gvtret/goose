# HANDOFF - goose LLVM-IR backend

## 2026-11-20 15:30 - variant tag dispatch

**Done:** Срез `5f5522d4` "llvm: variant tag dispatch". Fixed-mode enum как C-раскладка (`enum_ll_ty`: тег u8/u16 + payload-область, кеш по EnumInst через SmallVector - `{ty}` брейс-лист в `StructType::get` давал nelem=0!), variant-литералы (`StructLit::variant` или Dot `variantconst`), enum-локали/параметры (`w.enums`), payload-параметры кейсов (`w.payloads`, `s.r` через field_path_addr), dispatch-вызов (`emit_dispatch_call`: switch по тегу, аргументы до свитча, shared result channel) для одиночных int/bool/float/flat-struct результатов и growable (arms пишут в (dst, rl)); f64-результат функции добавлен в shell. `dispatch_resizable` переехал wrong -> fail (явный отказ "a multi-result variant dispatch call" на `let a, n, b = pair(c)`). Новая фикстура `ir_minimal_variant_dispatch` (74/226 pass).

**State:** Ветка `feature/llvm` (HEAD detached от cdd4726d после операций stash - проверить ветку!), HEAD `5f5522d4`, дерево чистое. Sweep: fail 152, wrong 0, pass 74. Обе регрессии all tests passed. ASCII-чек: 0.

**Next:** Мультирезультатный dispatch (`let a, n, b = pair(c)`): outs-каналы с типами (i64 out-pointer + struct/growable каналы), затем `dispatch_resizable` целиком. Далее: runtime-capacity `T[..]`, формат-хуки (gs_sp M3), inline slice-of-struct binding.

**Notes:** Питфоллы: `StructType::get(ctx, {x})` с брейс-листом одного элемента - использовать SmallVector; `getPrimitiveSizeInBits()`=0 для StructType; sweep-baseline-файлы в /tmp стираются при рестарте сервера - снимать baseline заново через stash перед сравнением; `strlit_glob` без терминатора; Debug-приём: printf с GS_IRDBG, чистка по точным блокам. Субагент-explorer дал полный разбор C-семантики dispatch (TryDispatch, EmitDispatch, Layouts - packed).

