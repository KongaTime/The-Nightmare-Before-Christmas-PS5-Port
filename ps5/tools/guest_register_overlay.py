#!/usr/bin/env python3
"""Overlay register declarations for a PS5 build without editing source checkouts."""

import argparse
import hashlib
import json
from pathlib import Path
import re

REGISTERS = {
    "eax", "ecx", "edx", "esp", "ebx", "esi", "edi", "fs_base", "seh_ebp",
    "ebp", "df", "mxcsr", "fp_stack", "fp_top", "fp_control_word", "fp_cmp", "fp_cc",
}
INITIAL_VALUES = {
    "fs_base": "XBOX_TIB_MAIN",
    "fp_control_word": "0x037Fu",
    "mxcsr": "0x1F80u",
    "fp_cc": "0x4000",
}
DECLARATION = re.compile(
    r"^([ \t]*)((?:extern[ \t]+)?(?:[A-Z]+_TLS|__thread|_Thread_local)"
    r"[ \t]+(?:uint32_t|uint16_t|int|double)[ \t]+)([^;\r\n]+);", re.MULTILINE
)
# Vector registers are declared with their own types, so the scalar pattern
# above never matched them and every use paid a full emulated-TLS resolve.
# They now live inside the grouped block (ps5_guest_regs.h), so these
# declarations and definitions are dropped.
VECTOR_DECL = re.compile(
    r"^[ \t]*(?:extern[ \t]+)?RECOMP_TLS[ \t]+(?:RecompXmm|RecompMmx)"
    r"[ \t]+([^;\r\n]+);", re.MULTILINE
)
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}


CHUNK = re.compile(r"recomp_[0-9]+[.]c")
FUNCTION = re.compile(r"^(void (?:sub_[0-9A-Fa-f]+(?:_gen)?|xbe_entry_point)\(void\)\n\{\n)", re.MULTILINE)


def use_local_register_pointer(source):
    first = FUNCTION.search(source)
    if not first:
        raise ValueError("Generated chunk contains no lifted functions")
    prefix = source[:first.start()] + "#undef OOGIE_GUEST_REGS\n#define OOGIE_GUEST_REGS (*oogie_regs)\n\n"
    if "void sub_001E522D(void)\n{" in source:
        header = Path(__file__).resolve().parents[1] / "platform/ps5_matrix_multiply.inc"
        prefix += f"#include {json.dumps(str(header))}\n\n"

    def local_registers(match):
        prologue = "    OogieGuestRegisters *const oogie_regs = &oogie_guest_regs;\n"
        # Hook the actual Xbox setter after its callers establish ECX/EDX.
        if match[0].startswith("void sub_001D2490_gen(void)"):
            prologue += "    if (oogie_hle_vec_setter()) return;\n"
        if match[0].startswith("void sub_001E522D(void)"):
            prologue += "    if (oogie_native_matrix(oogie_regs))\n        return;\n"
        fence = {"sub_001D78D0": 0, "sub_001D75F0": 1}.get(match[0].split()[1].split("(")[0])
        if fence is not None:
            prologue += (
                "    OogieFenceTiming oogie_fence __attribute__((cleanup(oogie_fence_end))) = "
                f"oogie_fence_begin({fence});\n"
            )
        return match[0] + prologue

    return prefix + FUNCTION.sub(local_registers, source[first.start():])


_LOCK_CALL = re.compile(
    r"\{ uint32_t _icall_target = MEM32\(0x25F8E(?:0|4)\);"
    r"[^{}]*?RECOMP_ICALL_SAFE_AT\(_icall_target, _icall_esp, 0x[0-9A-Fa-f]+u\); \}",
    re.DOTALL,
)


def elide_guest_lock(source):
    """Make the guest's device lock optional, at run time.

    Every hot setter brackets its work with two indirect calls through
    0x25F8E0/0x25F8E4 -- the acquire/release pair. Those are host stubs, and
    every guest thread already runs under the single guest CPU lock, so the
    pair cannot buy mutual exclusion between guest threads. Wrapping each call
    in a flag test skips two indirect dispatches per setter call (the family
    runs >10M times a tick) without touching the work inside the brackets.

    The argument is pushed by the preceding block. Restore the saved stack
    pointer when skipping the indirect call, as RECOMP_ICALL_SAFE_AT would.
    """
    def wrap(match):
        block = match[0].replace("RECOMP_ICALL_SAFE_AT(", "OOGIE_D3D_LOCK_CALL(")
        return "if (!oogie_hle_lock_off()) " + block + " else g_esp = _icall_esp;"

    prefix = (
        "#if defined(RECOMP_ABI_CHECK) || defined(RECOMP_ICALL_FEEDBACK) || defined(RECOMP_CALL_PROFILE)\n"
        "#define OOGIE_D3D_LOCK_CALL(va, sp, site) RECOMP_ICALL_SAFE_AT(va, sp, site)\n"
        "#else\n"
        "#define OOGIE_D3D_LOCK_CALL(va, sp, site) do { \\\n"
        "    if (!oogie_ps5_d3d_cs_direct(va, oogie_regs)) RECOMP_ICALL_SAFE_AT(va, sp, site); \\\n"
        "} while (0)\n"
        "#endif\n"
    )
    return prefix + _LOCK_CALL.sub(wrap, source)


def rewrite(source):
    def declaration(match):
        indent, qualifiers, declarators = match.groups()
        items = [item.strip() for item in declarators.split(",")]
        guest = []
        remaining = []
        for item in items:
            name = item.partition("=")[0].strip().partition("[")[0]
            if name.startswith("g_") and name[2:] in REGISTERS:
                guest.append((name[2:], item.partition("=")[2].strip()))
            else:
                remaining.append(item)
        if not guest:
            return match[0]
        if not qualifiers.startswith("extern"):
            for name, value in guest:
                expected = INITIAL_VALUES.get(name, "0")
                if value != expected and not (name == "fp_stack" and not value):
                    raise ValueError(f"Unexpected guest register definition: {match[0]}")
            if guest == [("fs_base", INITIAL_VALUES["fs_base"])]:
                fields = ", ".join(f".reg_{name} = {value}" for name, value in INITIAL_VALUES.items())
                return f"{indent}__thread OogieGuestRegisters oogie_guest_regs = {{ {fields} }};"
        return indent + qualifiers + ", ".join(remaining) + ";" if remaining else ""

    def vector_declaration(match):
        items = [item.strip() for item in match.group(1).split(",")]
        if all(item.partition("=")[0].strip().startswith(("g_xmm", "g_mm"))
               for item in items):
            return ""
        return match[0]

    return VECTOR_DECL.sub(vector_declaration, DECLARATION.sub(declaration, source))


DIRECT_CALL = re.compile(r"\b(sub_[0-9A-Fa-f]+(?:_gen)?)\(\)")
ABI_CALL = re.compile(r"\bRECOMP_ABI_CALL\((0x[0-9A-Fa-f]+u), (sub_[0-9A-Fa-f]+(?:_gen)?)\)")


def pass_guest_context(source, lifted):
    """Keep original entry points for the dispatcher and C runtime callers."""
    start = FUNCTION.search(source)
    assert start
    prefix, body = source[:start.start()], source[start.start():]
    declarations = set()

    def call(name):
        declarations.add(name)
        return f"oogie_ctx_{name}(oogie_regs)"

    def abi(match):
        address, name = match.groups()
        if name not in lifted:
            return match[0]
        declarations.add(name)
        return f"OOGIE_CONTEXT_CALL({address}, {name})"

    body = ABI_CALL.sub(abi, body)
    body = DIRECT_CALL.sub(lambda m: call(m[1]) if m[1] in lifted else m[0], body)
    body = body.replace("    OogieGuestRegisters *const oogie_regs = &oogie_guest_regs;\n", "")

    def definition(match):
        name = match[0].split()[1].split("(")[0]
        declarations.add(name)
        return (
            f"void {name}(void) {{ oogie_ctx_{name}(&oogie_guest_regs); }}\n\n"
            f"void oogie_ctx_{name}(OogieGuestRegisters *oogie_regs)\n{{\n"
        )

    body = FUNCTION.sub(definition, body)
    prototypes = "".join(
        f"void oogie_ctx_{name}(OogieGuestRegisters *);\n" for name in sorted(declarations)
    )
    dispatch_macro = (
        "#if defined(RECOMP_ABI_CHECK) || defined(RECOMP_CALL_PROFILE)\n"
        "#define OOGIE_CONTEXT_CALL(va, name) RECOMP_ABI_CALL(va, name)\n"
        "#else\n"
        "#define OOGIE_CONTEXT_CALL(va, name) oogie_ctx_##name(oogie_regs)\n"
        "#endif\n\n"
    )
    return prefix + prototypes + dispatch_macro + body


def reduce_d3d_diagnostics(source):
    old = (
        "    if (s_is_game_thread) {\n"
        "        extern volatile long g_oogie_presents;\n"
        "        int fo = oogie_fo_slot();\n"
        "        oogie_d3d_trace(fn);"
    )
    new = "    extern int oogie_ps5_d3d_light;\n" + old.replace(
        "if (s_is_game_thread)", "if (s_is_game_thread && !oogie_ps5_d3d_light)", 1
    )
    if source.count(old) != 1:
        raise ValueError("D3D frame-order and trace block did not match recomp_manual.c")
    return source.replace(old, new, 1)


def widen_visibility_projection(source):
    # sub_001B7100 builds a second projection at camera +0x170.
    # sub_001B7320 combines it with the view matrix for sub_001B6B70's
    # object visibility test. Only change its horizontal coefficient.
    start = source.index("loc_001B7165: ;")
    end = source.index("    MEMF(edi + 0x14) = xmm3.f[0];", start)
    old = "    MEMF(edi) = xmm3.f[0]; /* movss */"
    block = source[start:end]
    if block.count(old) != 1:
        raise ValueError("Horizontal visibility projection did not match recomp_0007.c")
    new = (
        "    extern float oogie_visibility_x_scale(void);\n"
        "    MEMF(edi) = xmm3.f[0] * oogie_visibility_x_scale();"
    )
    return source[:start] + block.replace(old, new) + source[end:]


def widen_level_visibility(source):
    # The level hierarchy and the individual props share this frustum.
    # 0x2602FC is the original 320-pixel horizontal half-extent.
    old = "    MEMF(0x3ECA50) = xmm1.f[0]; /* movss */"
    if source.count(old) != 1:
        raise ValueError("Level visibility half-width did not match recomp_0001.c")
    return source.replace(old, (
        "    extern float oogie_level_visibility_width_scale(void);\n"
        "    MEMF(0x3ECA50) = xmm1.f[0] * oogie_level_visibility_width_scale();"
    ))


def trace_display_gamma(source):
    anchor = "loc_001D89AA: ;\n"
    if source.count(anchor) != 1:
        raise ValueError("Xbox gamma ramp writer did not match recomp_0008.c")
    return source.replace(anchor, anchor + (
        "    extern void oogie_ps5_trace_gamma(uint32_t);\n"
        "    oogie_ps5_trace_gamma(MEM32(esp + 4));\n"
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pass-context", action="store_true")
    parser.add_argument("output", type=Path)
    parser.add_argument("sources", nargs="+", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    sources = sorted({
        file.resolve()
        for source in args.sources
        for file in (source.rglob("*") if source.is_dir() else [source])
        if file.is_file() and file.suffix in SOURCE_SUFFIXES
    })
    lifted = set()
    if args.pass_context:
        for source in sources:
            if CHUNK.fullmatch(source.name):
                text = source.read_text(encoding="utf-8", errors="surrogateescape")
                lifted.update(match[0].split()[1].split("(")[0] for match in FUNCTION.finditer(text))
    digest = hashlib.sha256()
    roots = []
    for source in sources:
        text = source.read_text(encoding="utf-8", errors="surrogateescape")
        if CHUNK.fullmatch(source.name):
            rewritten = elide_guest_lock(use_local_register_pointer(text))
            if source.name == "recomp_0001.c":
                rewritten = widen_level_visibility(rewritten)
            if source.name == "recomp_0008.c":
                rewritten = trace_display_gamma(rewritten)
            if source.name == "recomp_0007.c":
                rewritten = widen_visibility_projection(rewritten)
            if args.pass_context:
                rewritten = pass_guest_context(rewritten, lifted)
        else:
            rewritten = rewrite(text)
            if source.name == "recomp_manual.c":
                rewritten = reduce_d3d_diagnostics(rewritten)
        if rewritten == text:
            continue
        content = rewritten.encode("utf-8", errors="surrogateescape")
        key = hashlib.sha256(str(source).encode() + b"\0" + content).hexdigest()[:20]
        target = output / f"{key}-{source.name}"
        if not target.exists():
            target.write_bytes(content)
        digest.update(key.encode())
        roots.append({"type": "file", "name": str(source), "external-contents": str(target)})
    definitions = [root for root in roots if "oogie_patched/" in root["name"]
                   and root["name"].endswith("/xbox_memory_layout.c")]
    if len(definitions) != 1 or "__thread OogieGuestRegisters oogie_guest_regs" not in Path(
        definitions[0]["external-contents"]
    ).read_text(encoding="utf-8", errors="surrogateescape"):
        raise ValueError("The patched xbox_memory_layout.c did not define the grouped registers")
    overlay = output / f"registers-{digest.hexdigest()[:20]}.json"
    if not overlay.exists():
        overlay.write_text(json.dumps({
            "version": 0,
            "use-external-names": False,
            "roots": roots,
        }, indent=2) + "\n", encoding="utf-8")
    print(overlay)


if __name__ == "__main__":
    main()

