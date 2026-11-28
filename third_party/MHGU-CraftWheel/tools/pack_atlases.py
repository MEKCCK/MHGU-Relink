"""Pack a native v0.38 wheel tree without resampling any existing icon."""
from pathlib import Path
import argparse
import struct
import re
from PIL import Image


WIDTH, HEIGHT = 512, 256
SIZES = [16, 32, 64, 128]


def unpack(text, width, height):
    buf = bytearray()
    for match in re.finditer(r"\{(\d+),0x([a-fA-F0-9]{8})u\}", text):
        buf.extend(struct.pack("<I", int(match[2],16)) * int(match[1]))
    assert len(buf) == width * height * 4
    return Image.frombytes("RGBA", (width,height), bytes(buf))


class Page:
    def __init__(self):
        self.im = Image.new("RGBA", (WIDTH, HEIGHT))
        self.runs = [0] * WIDTH
        self.used_height = 0

    def slot(self, w, h):
        # List just the left edges of level sections and positions that end on one.
        edges = {0, WIDTH-w}
        for i in range(1, WIDTH):
            if self.runs[i] != self.runs[i-1]:
                edges.update((i, i-w))
        slots = [(max(self.runs[x:x+w]), x) for x in edges if 0 <= x <= WIDTH-w]
        y, x = min(slots)
        return None if y+h > HEIGHT else (y,x)

    def insert(self, im, y, x):
        w,h=im.size
        self.runs[x:x+w+2]=[y+h+2]*(w+2)
        self.used_height=max(self.used_height,y+h+2)
        # One outside texel on every edge isolates linear filtering from neighbours.
        self.im.paste(im,(x+1,y+1))
        for dx in (0,w+1):
            sx=0 if dx==0 else w-1
            self.im.paste(im.crop((sx,0,sx+1,h)),(x+dx,y+1))
        for dy in (0,h+1):
            sy=y+1 if dy==0 else y+h
            self.im.paste(self.im.crop((x,sy,x+w+2,sy+1)),(x,y+dy))


def pack(wheel):
    items_src=(wheel/"items_atlas.inc").read_text(encoding="utf-8")
    controller=unpack((wheel/"controller_atlas.inc").read_text(encoding="utf-8"),2048,2048)
    items=unpack(items_src,1024,1024)
    palette = items_src[items_src.index("static const unsigned item_palette"):].strip()
    entries, unique, groups = {}, {}, {}
    def sprite(key, image, level):
        side=image.width
        # Always keep a transparent guard inside the quad: edge interpolation
        # fades into that pixel rather than stopping at a cropped nonzero texel.
        chans=[b.getbbox() for b in image.split() if b.getbbox()]
        if not chans:
            entries[key]=((0,0,0,0),None,image)
            return
        x=max(0,min(b[0] for b in chans)-1);y=max(0,min(b[1] for b in chans)-1)
        x2=min(side,max(b[2] for b in chans)+1);y2=min(side,max(b[3] for b in chans)+1)
        crop=image.crop((x,y,x2,y2))
        k=(crop.size,crop.tobytes())
        if k not in unique:
            unique[k]=crop
            groups.setdefault(level,[]).append(k)
        entries[key]=((x,y,x2-x,y2-y),k,image)
    for i in range(54):
        for j,(edge,off) in enumerate(zip(SIZES,(224,192,128,0))):
            x=(i%8)*256+off;y=(i//8)*256
            sprite(("c",i,j),controller.crop((x,y,x+edge,y+edge)),j)
    for i in range(100):
        x=i%10*96;y=i//10*96
        sprite(("i",i),items.crop((x,y,x+96,y+96)),4)
    pages, locations=[],{}
    for level in sorted(groups):
        first=len(pages)
        for k in sorted(groups[level],key=lambda k: -k[0][1]*1024-k[0][0]):
            im=unique[k]
            for p in range(first,len(pages)):
                spot=pages[p].slot(im.width+2,im.height+2)
                if spot is not None: break
            else:
                pages.append(Page());p=len(pages)-1;spot=(0,0)
            y,x=spot
            pages[p].insert(im,y,x)
            locations[k]=(p,x+1,y+1)
    def record(key):
        box, k, original=entries[key]
        if k is None: return "{0}"
        left,top,w,h=box;p,x,y=locations[k]
        sampled=pages[p].im.crop((x,y,x+w,y+h))
        copy=Image.new("RGBA", original.size);copy.paste(sampled,(left,top))
        assert copy.tobytes()==original.tobytes(), key
        return "{"+",".join(map(str,(x,y,p,left,top,w,h)))+"}"
    with (wheel/"wheel_pages.inc").open("w",encoding="ascii",newline="\n") as f:
        f.write("typedef struct {unsigned short x,y;unsigned char page,left,top,w,h;} WheelSprite;\n")
        f.write("typedef struct {unsigned width,height;const unsigned *pixels;} AtlasPage;\n")
        shapes=[]
        for i,page in enumerate(pages):
            height=(page.used_height+7)//8*8
            shapes.append((WIDTH,height))
            f.write(f"static const unsigned wheel_page_{i}[]={{\n")
            raw=page.im.crop((0,0,WIDTH,height)).tobytes()
            for n in range(0,len(raw),4*128):
                f.write(",".join("0" if not v else hex(v) for v, in struct.iter_unpack("<I",raw[n:n+512]))+",\n")
            f.write("};\n")
        f.write("static const AtlasPage wheel_pages[]={\n")
        for i,(w,h) in enumerate(shapes):f.write(f" {{{w},{h},wheel_page_{i}}},\n")
        f.write("};\nstatic const WheelSprite control_sprites[54][4]={\n")
        for i in range(54):f.write("{"+",".join(record(("c",i,j)) for j in range(4))+"},\n")
        f.write("};\nstatic const WheelSprite item_sprites[100]={\n")
        for i in range(100):f.write(record(("i",i))+",\n")
        f.write("};\n"+palette+"\n")
    print(f"{len(pages)} texture pages, {sum(w*h*4 for w,h in shapes)} total pixel bytes, "
          f"{max(w*h*4 for w,h in shapes)} largest page; 216 control sprites and 100 items verified byte-for-byte")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    pack(parser.parse_args().directory)
