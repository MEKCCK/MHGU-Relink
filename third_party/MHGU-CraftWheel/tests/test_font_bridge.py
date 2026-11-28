"""Verify 32/64-bit field offsets and the real Forge HLE synchronization paths.

Pass one or more MHGURecomp source directories, a guest cimgui header, and
the ARM and native 64-bit C compilers. This test only compiles small probes.
"""
import argparse
from pathlib import Path
import re
import subprocess


def block(source, start):
    begin = source.index(start)
    return source[begin:source.index("\n}", begin)]


def needs(body, *tokens):
    for token in tokens:
        assert token in body, token


def check_offsets(cc, header, offsets):
    source = '#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS\n#include "cimgui.h"\n#include <stddef.h>\n'
    for (type_name, field), offset in offsets.items():
        source += f'_Static_assert(offsetof({type_name}, {field}) == {offset}, "{field}");\n'
    source += '_Static_assert(ImFontFlags_LockBakedSizes == 8, "flag");\n'
    subprocess.run([cc, "-std=c11", "-I", str(header.parent), "-x", "c", "-fsyntax-only", "-"],
                   input=source, text=True, check=True)


def verify(project, guest_cc, host_cc, guest_header):
    includes = (project / "runtime/forge_imgui_thunks.inc").read_text(encoding="utf-8")
    bridge = (project / "runtime/forge_imgui.c").read_text(encoding="utf-8")
    imgui = project / "third_party/cimgui"
    draw = (imgui / "imgui/imgui_draw.cpp").read_text(encoding="utf-8")
    guest = {("ImFont", "Flags"): 8, ("ImFontConfig", "OversampleH"): 51,
             ("ImFontConfig", "OversampleV"): 52}
    host = {}
    for (type_name, field), offset in guest.items():
        fields = block(includes, f"static const MField mf_{type_name}[] =")
        matches = [int(h) + offset - int(g) for g, h, n in
                   re.findall(r"\{(\d+), (\d+), (\d+), MF_RAW, 0\}", fields)
                   if int(g) <= offset < int(g) + int(n)]
        assert len(matches) == 1, (type_name, field, matches)
        host[type_name, field] = matches[0]
    check_offsets(guest_cc, guest_header, guest)
    check_offsets(host_cc, imgui / "cimgui.h", host)
    for name, tokens in {
        "ImFontConfig_ImFontConfig": ["m_out_new(r_, 18)"],
        "ImFontConfig_destroy": ["m_in(a0, 18)", "m_destroy(a0)"],
        "ImFontAtlas_AddFontFromFileTTF": ["m_in(a3, 18)", "ig_path(a1)", "m_out(r_, 11)"],
        "ImFont_GetFontBaked": ["m_in(a0, 11)", "m_after(a0)", "m_out(r_, 17)"],
        "ImDrawList_AddText_FontPtr": ["m_in(a1, 11)", "m_after(a1)", "ha1, a2,"],
        "igGetIO_Nil": ["m_out(r_, 38)"],
        "igGetFont": ["m_out(r_, 11)"],
        "igGetFontSize": ["memcpy(&c->r[0], &r_, 4)"],
    }.items():
        needs(block(includes, f'HLE_FUNC("{name}",'), *tokens)
    needs(block(bridge, "static void *m_in("), "if (m->shadow) push(m);", "return m->host;")
    needs(block(bridge, "static void m_after("), "pull(m)")
    needs(block(bridge, "static void push(Mirror *m) {"), "MF_RAW", "h[f->h + k] = s[f->g + k] = g[f->g + k]")
    needs(block(bridge, "static void pull(Mirror *m) {"), "memcpy(g + f->g, h + f->h, f->n)")
    needs(block(bridge, 'HLE_FUNC("igImFileOpen",'), "igImFileOpen(ig_path(ARG(0)), GP(ARG(1)))")
    needs(block(bridge, 'HLE_FUNC("igImFileRead",'), "ig_file(STK(2))", "igImFileRead(G2H(ARG(0)), size, count, f)")
    needs(block(bridge, 'HLE_FUNC("igImFileGetSize",'), "igImFileGetSize(f)")
    needs(block(bridge, 'HLE_FUNC("igImFileClose",'), "igImFileClose(f)")
    needs(draw, "font->Flags & ImFontFlags_LockBakedSizes", "ImFontAtlasBakedGetClosestMatch")
    print(f"{project.name}: guest and host field offsets, call signatures and sync OK")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest-cc", required=True)
    parser.add_argument("--host-cc", required=True)
    parser.add_argument("--guest-header", required=True, type=Path)
    parser.add_argument("projects", nargs="+", type=Path)
    args = parser.parse_args()
    for project in args.projects:
        verify(project, args.guest_cc, args.host_cc, args.guest_header)
