# Supporting OG, NG and AE from one DLL

Fallout 4 exists in three incompatible layouts, and the
[Dear-Modding-FO4](https://github.com/Dear-Modding-FO4/commonlibf4) fork of
CommonLibF4 lets a single DLL serve all three. This is the part of the
codebase you have to actually understand — everything else is plumbing.

**Status: AE is tested. OG and NG are experimental.** Development and all
in-game verification happen on AE — including the 60-test suite in `tests/`,
which runs against a live game. OG and NG build from the same source and are
believed correct by inspection of CommonLibF4's per-runtime tables, but nobody
has launched either of those builds with BNS installed. Treat them as
best-effort until field reports come in; the user-facing docs say exactly this,
so keep them in step.

## The three runtimes

| Index | Name | Versions |
|---|---|---|
| 0 | `kOG` | exactly 1.10.163 — the "downgraded" pre-update build |
| 1 | `kNG` | above 1.10.163 up to and including the NG latest (1.10.984) |
| 2 | `kAE` | anything newer (1.11.x, the Anniversary rebuild) |

The classification lives in `REX::FModule::GetRuntimeIndex()`
(`lib/CommonLibF4/lib/commonlib-shared/src/REX/FModule.cpp`), and it reads the
*executable's* file version, not F4SE's. Query it at runtime with
`REX::FModule::IsRuntimeOG()` / `IsRuntimeNG()` / `IsRuntimeAE()`.

## Why `COMMONLIB_RUNTIMECOUNT=3` matters

`xmake.lua` defines `COMMONLIB_RUNTIMECOUNT=3`, and that number is the array
length inside `REL::ID`, `REL::Offset` and `REL::VariantID`. Every address
CommonLibF4 resolves carries one entry per runtime, indexed by
`GetRuntimeIndex()`. A short list is padded with the *last* value given — a
two-element `REL::ID` does not leave AE unset, it silently reuses the NG
value. Do not lower `COMMONLIB_RUNTIMECOUNT`; it would truncate every one of
those arrays.

BNS itself never constructs a `REL::ID`/`REL::Offset` directly (grep `src/` —
there are none). Every address it uses comes from CommonLibF4's own
`RE::VTABLE::*` tables, which the library already resolves per-runtime. The
one thing BNS's *own* code hardcodes is vtable **slot indices** — see below.

## What F4SE is told

The `commonlibf4.plugin` xmake rule (in `lib/CommonLibF4/xmake.lua`) generates
`F4SEPlugin_Version` from `res/commonlibf4-plugin.cpp.in`, declaring:

```cpp
v.UsesAddressLibrary(true);
v.UsesAddressLibraryNG(true);
v.IsLayoutDependent(true);
v.IsLayoutDependentNG(true);
```

Because the plugin claims address-library use for both eras, F4SE will load
it on runtime versions it has never heard of instead of refusing on a version
mismatch. There is deliberately **no** `CompatibleVersions` list in
`src/main.cpp` — adding one would undo this and pin the DLL to whatever
versions happened to be enumerated (this is what BNS used to do, back when it
only supported NG/AE via `libxse/commonlibf4`).

`F4SEPlugin_Query` in `src/main.cpp` still rejects anything below 1.10.163,
which predates all three layouts.

## Preload vs Load

OG only ever calls `F4SEPlugin_Load`. NG and AE call `F4SEPlugin_Preload`
first, early enough to patch things before the game touches them, and then
`Load`. `src/main.cpp` exports both and routes them through one `InitPlugin`
guarded by a `static bool`, so setup (log, messaging listener, Papyrus
registration, serialization callbacks) happens exactly once no matter which
entry points the runtime uses.

## BNS's own vtable hooks — the actual risk

CommonLibF4's bindings are per-runtime by construction. What isn't automatic
is the two vtable hooks BNS installs itself, which hardcode a **slot index**
rather than going through a library-provided wrapper:

- `src/main.cpp` — `RE::VTABLE::Actor[0]`, vfunc index **`0x86`**
  (`Actor::Load3D`). This is the hook that routes every loaded actor into the
  swap pipeline; if it doesn't fire, BNS does nothing at all.
- `src/EditorIDLoader.hpp` — `T::VTABLE[0]`, vfunc indices **`0x3A`**/**`0x3B`**
  (`TESForm::GetFormEditorID`/`SetFormEditorID`), instantiated for 123 form
  types. This is the Hydra-absent fallback for EditorID lookups.

A vtable slot index is only safe to reuse across runtimes if Bethesda didn't
reorder virtual functions between builds. Before shipping OG support, I
checked the Dear-Modding-FO4 fork's own headers — the actual source of truth
for what layout the library assumes — and found `TESObjectREFR::Load3D`
declared at comment-index `86`, and `TESForm::GetFormEditorID`/
`SetFormEditorID` at `3A`/`3B`: exactly the values BNS already hardcodes.
I also searched the entire `include/RE/` tree for any place where the library
itself treats a vtable slot as runtime-dependent, and found none — the only
two `IsRuntimeOG()` call sites in all of `include/RE/` are for *data*
differences (a struct's byte offset, an array's element count), never a
vfunc slot. That's consistent with the community understanding that the
Next-Gen update mostly recompiled/relinked and appended new virtual functions
rather than reordering existing ones.

That's good evidence, not proof. Both hooks are shipped unchanged on the
assumption that `0x86`/`0x3A`/`0x3B` hold across OG/NG/AE. If a user reports
BNS doing nothing at all on OG, or EditorID-based rules failing to resolve
when Hydra is absent, start here.

## The practical rule for future code

Anything that reads a hardcoded address, a vtable index, or a struct member
offset is runtime-specific until proven otherwise. CommonLibF4's own bindings
already carry per-runtime IDs; it's *your* offsets — and vtable hooks — that
need scrutiny. There is no way to test this from Linux, and this project has
no way to launch the game at all (VM-only build) — a DLL that works on NG
says nothing about OG or AE.
