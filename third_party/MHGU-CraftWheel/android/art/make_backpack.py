"""Embed the original MHXX lower-screen item pouch, preserving its native pixels."""
import argparse
from pathlib import Path
import re
import struct
import zlib
from PIL import Image

TEXTURE = r"GUI\99_texture\custom_pnl_BM_NOMIP"
ETC_MODIFIERS = ((2,8,-2,-8),(5,17,-5,-17),(9,29,-9,-29),(13,42,-13,-42),
                 (18,60,-18,-60),(24,80,-24,-80),(33,106,-33,-106),(47,183,-47,-183))


def read_panel_texture(romfs):
    archive = (romfs/"arc/resident.arc").read_bytes()
    magic, version, count = struct.unpack_from("<4sHH",archive)
    if (magic, version) != (b"ARC\0", 17):
        raise ValueError("Expected a MHXX ARC v17 archive")
    for i in range(count):
        offset = 12 + i*80
        name, kind, size, _, start = struct.unpack_from("<64s4I",archive,offset)
        if kind == 0x241f5deb and name.rstrip(b"\0").decode("ascii") == TEXTURE:
            return zlib.decompress(archive[start:start+size])
    raise ValueError("Could not find the lower-screen panel texture")


def panel_pixel(texture, width, x, y):
    # CTR ETC1A4, with four 4x4 blocks per 8x8 tile, in Z order.
    tile = (y//8)*(width//8) + x//8
    block = tile*4 + ((y//4)&1)*2 + ((x//4)&1)
    alpha, color = struct.unpack_from("<QQ",texture,block*16)
    i = (x%4)*4 + y%4
    a = ((alpha >> (4*i)) & 15)*17
    if a == 0:
        return (0,0,0,0)
    hi = color >> 32
    second = (y%4 if hi & 1 else x%4) >= 2
    table = (hi >> (2 if second else 5)) & 7
    choice = (((color >> (i+16)) & 1) << 1) | ((color >> i) & 1)
    adjustment = ETC_MODIFIERS[table][choice]
    rgb = []
    for shift in (24,16,8):
        if hi & 2:
            value = (hi >> (shift+3)) & 31
            delta = (hi >> shift) & 7
            if second:
                value += delta if delta < 4 else delta-8
            base = (value << 3) | (value >> 2)
        else:
            value = (hi >> (shift + (0 if second else 4))) & 15
            base = value*17
        rgb.append(min(255,max(0,base+adjustment)))
    return (*rgb,a)


def extract(romfs):
    data = read_panel_texture(romfs)
    magic, tier0, dims, faces, fmt, depth, offset = struct.unpack_from("<4sIIBBHI",data)
    width,height = (dims>>6)&8191, dims>>19
    if (magic,tier0&255,width,height,faces,fmt,depth,dims&63,offset) != (b"TEX\0",166,512,512,1,12,1,1,0):
        raise ValueError("Unrecognized lower-screen ETC1A4 layout")
    # The illustrated アイテムポーチ in title.arc/ctmplModel confirms which panel this is.
    pixels = memoryview(data)[20:]
    icon = Image.new("RGBA",(72,72))
    for y in range(396,436):
        for x in range(72):
            icon.putpixel((x,16+y-396),panel_pixel(pixels,width,x,y))
    return icon


def embed(project, icon):
    path = project / "wheel" / "wheel_pages.inc"
    source = path.read_text(encoding="ascii")
    # Choose free space outside every sprite and its linear-filter guard band.
    sizes = re.findall(r"\{(\d+),(\d+),wheel_page_(\d+)\}", source)
    region = source[source.index("control_sprites"):source.index("item_palette")]
    sprites = [tuple(map(int,row)) for row in re.findall(
        r"\{(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\}",region)]
    for sw,sh,si in reversed(sizes):
        w,h,page = map(int,(sw,sh,si))
        used = [(x-1,y-1,x+rw+1,y+rh+1) for x,y,p,_,_,rw,rh in sprites if p==page]
        spot = next(((x+1,y+1) for y in range(0,h-97,2) for x in range(0,w-97,2)
                     if all(x>=r or y>=b or x+98<=l or y+98<=t for l,t,r,b in used)),None)
        if spot:
            break
    else:
        raise ValueError("The atlas needs space for a 96 x 96 backpack.")
    x,y = spot
    header = f"static const unsigned wheel_page_{page}[]={{\n"
    begin = source.index(header)+len(header)
    end = source.index("};", begin)
    pixels = [int(n,0) for n in re.findall(r"0x[0-9a-f]+|0",source[begin:end])]
    assert len(pixels)==w*h
    canvas = Image.new("RGBA", (96,96))
    canvas.paste(icon,(0,0))
    data = list(struct.unpack("<9216I",canvas.tobytes()))
    for i in range(96):
        offset=(y+i)*w+x
        pixels[offset:offset+96]=data[i*96:(i+1)*96]
    rows = [",".join(hex(p) if p else "0" for p in pixels[i:i+128])+",\n" for i in range(0,len(pixels),128)]
    path.write_bytes((source[:begin]+"".join(rows)+source[end:]).encode("ascii"))
    desc = f"static const WheelSprite backpack_sprite={{{x},{y},{page},0,0,72,72}};\n"
    (project / "wheel/backpack_sprite.inc").write_bytes(desc.encode("ascii"))


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--romfs",type=Path,help="Regenerate backpack.png from your own 3DS MHXX RomFS")
    args = cli.parse_args()
    art = Path(__file__).resolve().parent
    path = art/"backpack.png"
    if args.romfs:
        extract(args.romfs).save(path)
    with Image.open(path) as icon:
        if icon.size != (72,72) or icon.mode != "RGBA":
            raise ValueError("Expected the 72-pixel transparent panel sprite")
        embed(art.parent,icon)
