# Windows stub headers (test scaffolding only)

These headers exist so that the two "single file" mods can be *compiled*
(`g++ -fsyntax-only`) on a machine without the Windows SDK. They contain just
enough of the real Win32/COM API surface - with the same signatures the mods
rely on - to catch typos, missing members, wrong argument types and
use-before-declaration problems.

They are **not** part of the shipped product and must never be installed on
Windows: the mods are compiled by Windhawk's own toolchain, which has the real
headers.

## Rule: never invent a symbol

A stub is only useful if it is *exactly* as strict as the SDK. One revision
declared a fabricated `ASSOCSTR_SHELLIDLIST`, the local compile check passed,
and the user's real SDK then failed with

```
error: use of undeclared identifier 'ASSOCSTR_SHELLIDLIST'
```

When adding something here, copy the declaration from the Windows SDK
documentation, including enumerations in full - and never add a member that the
real header does not have.
