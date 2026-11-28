"""Check a distributed ARM NRO against the actual host dispatch tables."""
import argparse
from io import BytesIO
from pathlib import Path
import runpy
import struct
from zipfile import ZipFile

from elftools.elf.elffile import ELFFile


def android_entries(apk):
    with ZipFile(apk) as archive:
        data = archive.read("lib/arm64-v8a/libmain.so")
    elf = ELFFile(BytesIO(data))
    table = elf.get_section_by_name("hle_impls")
    addr, size = table["sh_addr"], table["sh_size"]
    pointers = bytearray(table.data())
    # RELR stores its addends in place. Older builds use explicit ELF64 RELA.
    relocs = elf.get_section_by_name(".rela.dyn")
    if relocs:
        symbols = elf.get_section(relocs["sh_link"])
        for entry in relocs.iter_relocations():
            offset = entry["r_offset"] - addr
            if 0 <= offset < size:
                value = entry["r_addend"]
                if entry["r_info_type"] == 257:  # R_AARCH64_ABS64
                    value += symbols.get_symbol(entry["r_info_sym"])["st_value"]
                else:
                    assert entry["r_info_type"] == 1027  # R_AARCH64_RELATIVE
                struct.pack_into("<Q", pointers, offset, value)
    segments = [seg for seg in elf.iter_segments() if seg["p_type"] == "PT_LOAD"]
    def resolve(address, executable=False):
        for seg in segments:
            delta = address - seg["p_vaddr"]
            if 0 <= delta < seg["p_filesz"] and (not executable or seg["p_flags"] & 1):
                return seg["p_offset"] + delta
        raise ValueError(f"Unmapped ELF address {address:#x}")
    return resolve_entries(data, pointers, resolve)


def windows_entries(exe):
    data = exe.read_bytes()
    pe, = struct.unpack_from("<I", data, 60)
    assert data[pe:pe+4] == b"PE\0\0"
    count, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20b
    base, = struct.unpack_from("<Q", data, optional + 24)
    sections = {}
    for i in range(count):
        off = optional + optional_size + 40*i
        name = data[off:off+8].rstrip(b"\0")
        vsize, rva, raw_size, raw_offset = struct.unpack_from("<4I", data, off+8)
        flags, = struct.unpack_from("<I", data, off+36)
        sections[name] = (rva, min(vsize, raw_size), raw_offset, flags)
    def resolve(address, executable=False):
        for rva, size, offset, flags in sections.values():
            delta = address - base - rva
            if 0 <= delta < size and (not executable or flags & 0x20000000):
                return offset + delta
        raise ValueError(f"Unmapped PE address {address:#x}")
    _, size, offset, _ = sections[b".hle"]
    return resolve_entries(data, data[offset:offset+size], resolve)


def resolve_entries(binary, pointers, resolve):
    entries = set()
    for name, fn in struct.iter_unpack("<QQ", pointers):
        if name == 0 and fn == 0:
            continue
        resolve(fn, executable=True)
        start = resolve(name)
        entries.add(binary[start:binary.index(b"\0", start, start+2048)])
    return entries


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("nro", type=Path)
    cli.add_argument("hosts", nargs="+", type=Path)
    cli.add_argument("--parser", type=Path, default=Path(__file__).resolve().parents[1]/"isolate_runtime.py")
    args = cli.parse_args()
    rows = runpy.run_path(str(args.parser))["table"](args.nro.read_bytes())[1]
    required = {row[3] for row in rows if row[3] and not row[5] and row[4] >> 4 != 2}
    optional = {row[3] for row in rows if row[3] and not row[5] and row[4] >> 4 == 2}
    assert required
    for host in args.hosts:
        entries = android_entries(host) if host.suffix.lower() == ".apk" else windows_entries(host)
        assert not required - entries, (host.name, sorted(required - entries))
        print(f"{host.name}: {len(required)}/{len(required)} imports required imports registered, {len(optional & entries)}/{len(optional)} optional")
