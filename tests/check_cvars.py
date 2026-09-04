#!/usr/bin/env python3
"""Static cvar-consistency analyzer for the skate3 C++ tree.

Catches the class of bug that broke the last build before compilation:

  REXCVAR_GET/SET(cvar) used in a TU where the cvar's storage accessor is
  not visible (no REXCVAR_DECLARE in the same TU, and the TU is not the one
  that DEFINEs it) -> `use of undeclared identifier FLAGS_<cvar>_storage_`.

Checks, per cvar name across the whole tree:

  1. GLOBAL_DEFINE: exactly ONE REXCVAR_DEFINE_* must exist across all TUs.
       - 0  -> the cvar is used but never defined (link error / undefined).
       - >1 -> duplicate definition (ODR / link-redefinition error).
  2. TU_VISIBLE: for every TU that calls REXCVAR_GET/SET(name) or
     REXCVAR_DECLARE(name), the accessor must be visible in THAT TU, i.e. the
     TU either contains its own REXCVAR_DECLARE(name) or is the TU that
     DEFINEs name.
  3. DECL_WITHOUT_USE: a REXCVAR_DECLARE whose cvar is never GET/SET/DEFINE
     anywhere (unused declaration - warning, not an error).
  4. DEFINE_WITHOUT_DECL_OTHER: FYI only (headers declare, cpp defines) -
     not enforced, matching the repo's convention.

Only direct-arg forms are matched (name is a plain identifier). Comma-heavy
macros are fine since GET/SET take a single name argument.

TRIAGE: verbose opt-in prints DEREF/FIX hints. No build required.

Usage:
    python3 tests/check_cvars.py [--dir src] [--verbose] [--json]
Exit code 0 = clean, 1 = hard errors (undefined / duplicate / undeclared use).
"""

import argparse
import json
import os
import re
import sys


# REXCVAR_GET/SET with a single identifier arg (not a comma-expression).
USAGE_RE = re.compile(r"REXCVAR_(?:GET|SET)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")
DECLARE_RE = re.compile(
    r"REXCVAR_DECLARE\s*\(\s*[A-Za-z_][A-Za-z0-9_:<>]*\s*,\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\)")
DEFINE_RE = re.compile(
    r"REXCVAR_DEFINE_(?:BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|STRING|COMMAND)"
    r"\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,")

COMMENT_OR_LIT = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'", re.DOTALL)


def gather(root: str) -> list[str]:
    files = []
    for dirpath, _d, fns in os.walk(root):
        for fn in fns:
            if fn.endswith((".cpp", ".h", ".hpp", ".mm")):
                files.append(os.path.join(dirpath, fn))
    return sorted(files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default="src")
    ap.add_argument("--sdk", default="third_party/rexglue-sdk")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    root = os.path.abspath(args.dir)
    if not os.path.isdir(root):
        print(f"error: {root} is not a directory")
        return 1
    files = gather(root)

    # cvar -> set(files that use/declare) / set(files that define)
    uses: dict[str, set[str]] = {}
    decls: dict[str, set[str]] = {}
    defs: dict[str, set[str]] = {}
    # cvars declared in any header (src or sdk) -> globally visible
    header_declared: set[str] = set()
    # file -> list of (cvar, lineno) usages for FIX hints
    file_usages: dict[str, list[tuple[str, int]]] = {}

    for fp in files:
        with open(fp, "r", errors="ignore") as f:
            text = f.read()
        strip = COMMENT_OR_LIT.sub("", text)
        # Collapse whitespace so multi-line macro invocations match the regexes
        # (REXCVAR_DEFINE_*(name may be on the line after the macro keyword).
        squash = re.sub(r"\s+", " ", strip)
        for m in USAGE_RE.finditer(squash):
            c = m.group(1)
            uses.setdefault(c, set()).add(fp)
            # re-find line number by locating the token in the stripped source
        for m in DECLARE_RE.finditer(squash):
            decls.setdefault(m.group(1), set()).add(fp)
            # A cvar declared in ANY header is visible to any TU that includes
            # the header -> treat header-declared cvars as globally visible.
            if fp.endswith((".h", ".hpp")):
                header_declared.add(m.group(1))
        for m in DEFINE_RE.finditer(squash):
            defs.setdefault(m.group(1), set()).add(fp)
        # line numbers for usage hints: scan the non-squashed stripped text
        for line, src in enumerate(strip.splitlines(), 1):
            for m in USAGE_RE.finditer(src):
                c = m.group(1)
                file_usages.setdefault(fp, []).append((c, line))

    all_cvars = set(uses) | set(decls) | set(defs)

    # Scan the rexglue SDK tree for REXCVAR_DEFINE_* / REXCVAR_DECLARE_* so SDK
    # owned cvars (used by the app but defined/declared in the SDK's own trees)
    # are not misreported. Header declares become globally visible; SDK DEFINEs
    # satisfy the "must be defined" check.
    sdk_root = os.path.abspath(args.sdk)
    if os.path.isdir(sdk_root):
        for fp in gather(sdk_root):
            try:
                with open(fp, "r", errors="ignore") as f:
                    strip = COMMENT_OR_LIT.sub("", f.read())
            except OSError:
                continue
            squash = re.sub(r"\s+", " ", strip)
            for m in DEFINE_RE.finditer(squash):
                defs.setdefault(m.group(1), set()).add("<sdk>")
            for m in DECLARE_RE.finditer(squash):
                if fp.endswith((".h", ".hpp")):
                    header_declared.add(m.group(1))

    errors: list[str] = []
    warns: list[str] = []

    for cvar in sorted(all_cvars):
        def_files = defs.get(cvar, set())
        decl_files = decls.get(cvar, set())
        use_files = uses.get(cvar, set())

        # 1. exactly one global DEFINE
        if not def_files:
            errors.append(
                f"UNDEFINED  '{cvar}': used/declared but never REXCVAR_DEFINE_* "
                f"anywhere (used in {len(use_files)}, declared in {len(decl_files)})")
            continue  # nothing else can be validated
        if len(def_files) > 1:
            errors.append(
                f"DUPLICATE  '{cvar}': REVXCVAR_DEFINE_* in more than one TU: "
                + ", ".join(sorted(def_files)))

        # 2. every using TU must have the accessor visible: either it DEFINEs
        #    the cvar, it contains a REXCVAR_DECLARE, or the cvar is declared in
        #    a header (globally visible via include).
        if cvar in header_declared:
            continue
        for tu in sorted(use_files):
            if tu in def_files:  # the defining TU always has the accessor
                continue
            if tu not in decl_files:
                # line numbers where THIS cvar is used in this TU
                lines = sorted(set(ln for c, ln
                                   in file_usages.get(tu, []) if c == cvar))
                needs = "REXCVAR_GET/SET"
                errors.append(
                    f"NOTVISIBLE '{cvar}': {tu} uses {needs} at "
                    f"{', '.join(str(ln) for ln in lines[:8])}"
                    f"{'...' if len(lines) > 8 else ''} "
                    f"but has no REXCVAR_DECLARE (or header decl) and is not "
                    f"the defining TU (defined in {', '.join(sorted(def_files))})")

        # 3. DECLARE without any DEFINE is already UNDEFINED; skip.

    # 3 (cont). declarations / defines that are never GET/SET (FYI warn)
    for cvar in sorted(decls):
        if cvar not in uses and cvar not in defs:
            warns.append(
                f"UNUSED_DECL '{cvar}': REXCVAR_DECLARE present but never "
                f"GET/SET/DEFINE -- likely dead or a pure extern header decl")

    rc = 0
    if errors:
        rc = 1
        print("=== CVAR ERRORS (must fix) ===")
        for e in errors:
            print("  " + e)
    if warns:
        print("=== CVAR WARNINGS (FYI) ===")
        for w in warns:
            print("  " + w)
    if args.verbose and file_usages:
        print("\n=== USAGE MAP (fix hints) ===")
        for fp, lst in sorted(file_usages.items()):
            print(f"  {os.path.relpath(fp, os.getcwd())}:")
            for c, ln in lst:
                print(f"      L{ln}: {c}")
    if not errors and not warns:
        print("[check_cvars] CLEAN")
    return rc


if __name__ == "__main__":
    sys.exit(main())
