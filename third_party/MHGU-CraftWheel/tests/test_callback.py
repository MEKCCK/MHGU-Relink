"""Replay touch, controller and file operations through release ARM NRO callbacks."""
import argparse
from hashlib import sha256
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_FPEXC)

parser = argparse.ArgumentParser()
parser.add_argument("project", type=Path)
parser.add_argument("--nro", type=Path, help="Release NRO, including one with isolated import names")
parser.add_argument("--font",type=Path,help="Packaged font file for the port's file bridge")
parser.add_argument("--font-mode",choices=("present","missing","truncated"),default="present")
parser.add_argument("--portable-bridge",action="store_true",help="Bind the Android host's optional pointer capture API")
parser.add_argument("--stock-cloud", nargs="?", const="both", choices=("both", "pad", "gear"),
                    help="Replay stock CLOUD with no optional pointer APIs or absolute in-game touch")
parser.add_argument("--scale-only", action="store_true", help="Replay Android wheel size editing and migration")


parser.add_argument("--prowler-play",action="store_true",help="Replay automatic Prowler selection and mission input")
parser.add_argument("--prowler-settings", action="store_true", help="Replay the independent Prowler settings menu")
parser.add_argument("--settings-entry", action="store_true", help="Exercise the PC native menu handoff using mixed input")
parser.add_argument("--preset-chord", action="store_true", help="Verify the configured preset chord and its rendered icon")
args = parser.parse_args()
if args.stock_cloud and args.portable_bridge:
    parser.error("The stock CLOUD host has no portable pointer bridge")
wheel = args.project / "wheel"
image = (args.nro or wheel / "gu_craft_wheel.nro").read_bytes()
BASE, MAIN, HEAP, STUB = 0x100000, 0x4000000, 0x50000000, 0x60000000
STOP, STACK = HEAP+0xff000, HEAP+0xee000
IO, PAD, DTI, VT, FILE, FVT = [HEAP+n for n in (0x1000,0x5000,0x6000,0x6100,0x6200,0x6300)]
cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
for address, size in ((BASE, (len(image)+0x3fff)&~0xfff), (MAIN, 0x2000000), (HEAP, 0x100000), (STUB, 0x10000)):
    cpu.mem_map(address, size)
cpu.mem_write(BASE, image)
cpu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]


def read(address, fmt):
    return struct.unpack(fmt, cpu.mem_read(address, struct.calcsize(fmt)))


def put(address, fmt, *values):
    cpu.mem_write(address, struct.pack(fmt, *values))


def string(address):
    return bytes(cpu.mem_read(address, 512)).split(b"\0", 1)[0].decode("utf8")


calls = {}
with (wheel / "wheel.elf").open("rb") as stream:
    elf = ELFFile(stream)
    symbols = {symbol.name:BASE+symbol["st_value"] for symbol in elf.get_section_by_name(".symtab").iter_symbols()}
    page_table=elf.get_section_by_name(".symtab").get_symbol_by_name("wheel_pages")[0]
    page_count=page_table["st_size"]//12
    symbols_dynamic = elf.get_section_by_name(".dynsym")
    strings_start = elf.get_section_by_name(".dynstr")["sh_addr"]
    # The native pack namespaces its imports, preserving all code offsets.
    text = elf.get_section_by_name(".text")
    assert image[0x100:text.data_size]==text.data()[0x100:]
    for section in elf.iter_sections():
        if section["sh_type"]!="SHT_REL":
            continue
        for relocation in section.iter_relocations():
            where = BASE+relocation["r_offset"]
            kind = relocation["r_info_type"]
            if kind==23:
                put(where, "<I", BASE+read(where,"<I")[0])
            elif kind in (21,22):
                dest = STUB+4*len(calls)
                symbol = symbols_dynamic.get_symbol(relocation["r_info_sym"])
                start = strings_start+symbol["st_name"]
                disk_name = image[start:image.index(0,start)].decode()
                canonical = symbol.name
                missing_pointer = canonical.startswith("forge_wheel_") and not args.portable_bridge
                missing_mouse_free = args.stock_cloud and canonical == "forge_input_setMouseFree"
                if missing_pointer or missing_mouse_free:
                    assert symbol["st_info"]["bind"]=="STB_WEAK"
                    put(where,"<I",0)
                    continue
                isolated = ("wcore_"+canonical[6:] if canonical.startswith("forge_") else
                            "wg"+canonical[2:] if canonical.startswith("ig") else
                            "Wm"+canonical[2:] if canonical.startswith("Im") else canonical)
                assert disk_name in (canonical, isolated), (canonical, disk_name)
                calls[dest] = canonical
                put(where, "<I", dest)
            else:
                raise AssertionError(kind)

def var(name, fmt="<I"):
    return read(symbols[name], fmt)[0]


def call(address, *values, stop=STOP, limit=400000):
    for reg, value in zip(regs, values):
        cpu.reg_write(reg,value)
    cpu.reg_write(UC_ARM_REG_SP,STACK)
    cpu.reg_write(UC_ARM_REG_LR,stop)
    cpu.emu_start(address,stop,count=limit)
    assert cpu.reg_read(UC_ARM_REG_PC)==stop, ("ARM execution limit",hex(address),hex(cpu.reg_read(UC_ARM_REG_PC)))


def game_function(offset, name):
    calls[MAIN+offset] = name


for offset,word in ((0x194474,0xe92d47f0),(0x19b7fc,0xe92d4ff0),(0x3f8634,0xe590001c),
                    (0x560d3c,0xe59f0080),(0x13f0af0,0xe28fc604),(0x13f0b20,0xe28fc604),
                    (0x3de4b0,0xe92d4800),(0x3de4b4,0xe59f0034),(0x3de4f8,0xe3a02010),(0x3de4fc,0xe3a00040),
                    (0x792728,0xe92d4070),(0x79272c,0xe1a04000),
                    (0x66fecc,0xe92d4830),(0x66ff3c,0xe92d4070),(0x66e3ac,0xe92d48f0),
                    (0x66f868,0xe92d48f0),(0x66f8e8,0xebfb7d6a),(0x625138,0xe92d4bf0),
                    (0x7aa47c,0xe59f1010),(0x677d28,0xe3a0300d),(0x78f7a8,0xe3a03010)):
    put(MAIN+offset,"<I",word)
game_function(0x3f8634,"quest")
game_function(0x7aef68,"dti")
put(MAIN+0x18584b8, "<I", HEAP+0xa000)
put(MAIN+0x188ca48, "<I", HEAP+0xc000)
put(DTI,"<I",VT)
put(FILE,"<I",FVT)
for table, method, name in ((VT,2,"factory"),(FVT,1,"destroy"),(FVT,6,"open"),(FVT,7,"close"),
                            (FVT,8,"read"),(FVT,12,"write"),(FVT,15,"size")):
    dest = STUB+0x1000+len(calls)*4
    calls[dest] = name
    put(table+method*4,"<I",dest)
put(IO+8,"<4f",1280,720,1,1)
now = 0
quest = False
held = 0
touch = None
left_stick=(0,0)
right_stick=(0,0)
files = {}
file_name = ""
pad_hooks = []
menu_hooks = {}
action_hooks = {}
action_targets = {0x3de4b0:"gameplay",0x3de4f8:"menu"}
action_calls = []
town_observed = []
texts = []
font_atlas, face, config, context = (HEAP+0x31000, HEAP+0x32000, HEAP+0x33000, HEAP+0x34000)
put(IO+44, "<I", font_atlas)
cache_sizes = []
pages = set()
font_loads = []
port="ImTextureData_ImTextureData" in calls.values()
assert port or args.font_mode=="present"
font_handle=0xef000000
default_face=IO+0x200
font_bytes=bytes.fromhex("74746366000100000000000100000010")+bytes(512)
if args.font:font_bytes=args.font.read_bytes()
if args.font_mode=="truncated":font_bytes=font_bytes[:7]
staging=0x70000000
page_slot=512*1024
if port:cpu.mem_map(staging,page_count*page_slot)
textures={}
expected_pages={}
for i in range(page_count):
    w,h,pixels=read(symbols["wheel_pages"]+12*i,"<III")
    assert 0<w*h*4<=page_slot
    expected_pages[pixels]=(w,h,sha256(cpu.mem_read(pixels,w*h*4)).digest())
images=[]
cursor_requests=[]
pc_mouse=None
pc_keys=set()
image_geometry=[]
wheel_rings=[]
red_cross_lines=[]
host_panel=False
host_click_button=False
host_popup=False
host_focused=True
host_finger=None
host_pointer_position=(-1e20,-1e20)
host_pointer_calls=0
bridge_region=(0,0,0,0)
bridge_lift=None
bridge_cancel=False
bridge_seen=[]


def callback(vm,address,size,data):
    global file_name,host_popup,host_pointer_calls,bridge_region,host_pointer_position
    name = calls[address]
    a,b,c,d = [cpu.reg_read(reg) for reg in regs]
    result = 0
    if name=="forge_wheel_setInputArea":
        assert a in (0,1,2)
        x,y,r=struct.unpack("<fff",struct.pack("<III",b,c,d))
        assert r==36.0 and 100<=x<=1200 and 100<=y<=620
        bridge_region=(a,x,y,r);bridge_seen.append(bridge_region)
    elif name=="forge_wheel_getPointer":
        assert bridge_region[0] in (1,2)
        if bridge_cancel:result=2
        elif touch:
            put(a,"<f",touch[0]);put(b,"<f",touch[1]);result=1
        elif bridge_lift:
            put(a,"<f",bridge_lift[0]);put(b,"<f",bridge_lift[1])
    elif name=="forge_graphics_isMenuVisible":result=int(host_panel)
    elif name=="igButton":
        assert "CW039Editor" in string(a)
        result=int(host_click_button)
    elif name=="igOpenPopup_Str":
        assert host_panel and "GUCraftWheel039Modal" in string(a)
        host_popup=True
    elif name=="igBeginPopupModal":
        assert "GUCraftWheel039Modal" in string(a)
        assert (c & (1<<9))==0  # gets mouse input and blocks the window below
        result=int(host_popup)
    elif name=="igCloseCurrentPopup":host_popup=False
    elif name in ("igSetNextWindowPos","igSetNextWindowSize","igSetNextWindowBgAlpha","igEndPopup"):pass
    elif name=="igIsWindowFocused":result=int(host_focused)
    elif name=="igIsMouseDown_Nil":result=int(host_finger is not None)
    elif name=="igGetMousePos":
        if host_finger is not None:
            host_pointer_position = host_finger
        put(a,"<ff",*host_pointer_position);result=a;host_pointer_calls+=1
    elif name=="forge_mem_getMainTextAddr":result=MAIN
    elif name=="forge_hook_create":
        original = STUB+0xa000+len(calls)*4
        if b in (MAIN+0x13f0af0,MAIN+0x13f0afc,MAIN+0x13f0b20) or "GetNpadState" in calls.get(b,""):
            pad_hooks.append(c)
            calls[original]="pad"
        elif b-MAIN in action_targets:
            view=action_targets[b-MAIN]
            action_hooks[view]=c
            calls[original]=view+"_translator"
        elif b==MAIN+0x792728:
            menu_hooks[b-MAIN]=c
            calls[original]="town_dispatch"
        else:
            menu_hooks[b-MAIN]=c
            calls[original]="menu_original"
        if d:put(d,"<I",original)
        put(a,"<6I",b,c,original,0,0,0)
        result=a
    elif name=="quest":result=int(quest)
    elif name in ("gameplay_translator","menu_translator"):
        action_calls.append((name,a,b))
        # Distinct stub results check both chaining and return-value forwarding.
        result=b|(0x1000000 if name=="gameplay_translator" else 0x2000000)
    elif name=="town_dispatch":
        pad,=read(MAIN+0x188c670,"<I")
        town_observed.append((a,read(pad+0x6bf4,"<4I") if pad else None))
        result=0x739
    elif name=="igGetIO_Nil":result=IO
    elif name=="igGetCurrentContext":result=context
    elif name=="igImFileOpen":
        assert port and string(a).endswith("/gu_craft_wheel_font.ttc") and string(b)=="rb"
        result=font_handle if args.font_mode!="missing" else 0
    elif name=="igImFileGetSize":
        assert a==font_handle
        result=len(font_bytes)
        cpu.reg_write(UC_ARM_REG_R1,0)
    elif name=="igImFileRead":
        count,file=read(cpu.reg_read(UC_ARM_REG_SP),"<QI")
        assert c==1 and d==0 and file==font_handle
        content=font_bytes[:count]
        cpu.mem_write(a,content)
        result=len(content)
        cpu.reg_write(UC_ARM_REG_R1,0)
    elif name=="igImFileClose":
        assert a==font_handle
        result=1
    elif name=="igGetFontSize":
        assert args.font_mode!="present"
        result=struct.unpack("<I",struct.pack("<f",18))[0]
    elif name=="ImFontConfig_ImFontConfig":result=config
    elif name=="ImFontConfig_destroy":assert a==config
    elif name=="ImFontAtlas_AddFontFromFileTTF":
        assert a==font_atlas and d==config and read(config+51,"<bb")==(1,1)
        assert string(b).endswith("/gu_craft_wheel_font.ttc")
        font_loads.append(b)
        if not port:
            put(font_atlas+84,"<III",1,1,face+512)
            put(face+512,"<I",face)
        result=face
    elif name=="ImFont_GetFontBaked":
        assert a==face
        cache_sizes.append(struct.unpack("<f",struct.pack("<I",b))[0])
        result=face+1024
    elif name=="igMemAlloc":
        assert a<8192
        result=HEAP+0x35000
    elif name=="igAddContextHook":
        assert not port and a==context
        result=1
    elif name=="ImTextureData_ImTextureData":
        assert port and len(textures)<page_count
        # The host texture handle is deliberately unmapped in guest memory.
        result=0xf0000000+len(textures)*16
        textures[result]={"stage":staging+len(textures)*page_slot}
    elif name=="ImTextureData_Create":
        assert port and a in textures and b==0 and 0<c*d*4<=page_slot
        textures[a].update(w=c,h=d)
    elif name=="ImTextureData_GetPixels":
        assert port and a in textures
        result=textures[a]["stage"]
    elif name=="igRegisterUserTexture":
        if port:
            tex=textures[a]
            w,h=tex["w"],tex["h"]
            assert (w,h,sha256(cpu.mem_read(tex["stage"],w*h*4)).digest()) in expected_pages.values()
        else:
            w,h=read(a+28,"<II")
            pixels,=read(a+40,"<I")
            assert expected_pages[pixels][:2]==(w,h)
            put(a+76,"<H",1)
        assert a not in pages
        pages.add(a)
    elif name=="ImTextureData_GetTexRef":
        assert b in pages
        put(a, "<IIII", b,0,0,0)
        result=a
    elif name in ("igGetMainViewport","igGetForegroundDrawList_Nil","igGetFont"):result=IO+0x200
    elif name=="gu_wheel_cursor_v1":
        cursor_requests.append((a,b,c))
        result=int(a==1)
    elif name=="forge_input_getMouseMove":
        put(a,"<f",1 if pc_mouse else 0);put(b,"<f",0)
    elif name=="forge_input_getMouse":
        if pc_mouse:
            if a:put(a,"<f",pc_mouse[0])
            if b:put(b,"<f",pc_mouse[1])
            if c:put(c,"<I",pc_mouse[2])
            result=1
    elif name in ("forge_input_isKeyPressed","forge_input_isKeyDown"):result=int(a in pc_keys)
    elif name=="forge_input_isDown":result=int(bool(held&(1<<a)))
    elif name in ("forge_input_getTouch","forge_input_getStickL","forge_input_getStickR"):
        if name.endswith("Touch"):
            value = None if args.stock_cloud or args.portable_bridge or host_panel else touch
        else:
            value = left_stick if name.endswith("L") else right_stick
        if value:
            put(a,"<f",value[0]);put(b,"<f",value[1])
            result=1
    elif name=="pad" or "GetNpadState" in name:
        put(a,"<QQiiiiI",123,held|0xf00000,111,222,333,-444,1)
    elif name=="nnosGetSystemTick" or name=="nnosGetSystemTickFrequency":
        result=now if name=="nnosGetSystemTick" else 1000
        cpu.reg_write(UC_ARM_REG_R1,0)
    elif name=="dti":result=DTI
    elif name=="factory":result=FILE
    elif name=="forge_fs_mountSaveData":result=1
    elif name=="open":
        file_name=string(b)
        if c==0x13:files[file_name]=b""
        result=int(file_name in files)
    elif name=="size":result=len(files[file_name])
    elif name=="read":
        content=files[file_name][:c]
        cpu.mem_write(b,content)
        result=len(content)
    elif name=="write":
        files[file_name]=bytes(cpu.mem_read(b,c))
        result=c
    elif "MountSdCard" in name:assert string(a)=="gucw"
    elif "CreateFile" in name:
        file_name=string(a)
        if file_name in files:result=0x402
        else:files[file_name]=b""
    elif "OpenFile" in name:
        file_name=string(b)
        if file_name in files:put(a,"<I",123)
        else:result=0x202
    elif "GetFileSize" in name:put(a,"<q",len(files[file_name]))
    elif "SetFileSize" in name:files[file_name]=bytes(c)
    elif "ReadFile" in name or "WriteFile" in name:
        ptr,count=read(cpu.reg_read(UC_ARM_REG_SP),"<II")
        if "ReadFile" in name:
            content=files[file_name][:count]
            cpu.mem_write(ptr,content)
            put(a,"<I",len(content))
        else:files[file_name]=bytes(cpu.mem_read(ptr,count))
    elif name=="ImDrawList_AddText_FontPtr":
        if args.font_mode=="present":
            assert b==face and read(face+8,"<I")[0]&8 and cache_sizes==[24.0,36.0]
        else:
            assert b==default_face and not cache_sizes and not font_loads
            assert struct.unpack("<f",struct.pack("<I",c))[0]==18
        texts.append(string(read(cpu.reg_read(UC_ARM_REG_SP)+8,"<I")[0]))
    elif name=="ImDrawList_AddImage":
        assert c in pages
        uv=read(cpu.reg_read(UC_ARM_REG_SP)+24,"<ffff")
        assert 0<=uv[0]<uv[2]<=1 and 0<=uv[1]<uv[3]<=1
        images.append(c)
        if args.prowler_play:
            sp=cpu.reg_read(UC_ARM_REG_SP)
            image_geometry.append((*read(sp+8,"<4f"),read(sp+40,"<I")[0]>>24))
    elif name=="ImDrawList_AddCircle" and args.prowler_play:
        x,y,r=struct.unpack("<3f",struct.pack("<3I",b,c,d))
        color,segments,thickness=read(cpu.reg_read(UC_ARM_REG_SP),"<IIf")
        wheel_rings.append((x,y,r-.5,color,segments,thickness,len(images)))
    elif name=="ImDrawList_AddLine" and args.prowler_play:
        color=read(cpu.reg_read(UC_ARM_REG_SP)+4,"<I")[0]
        if color&0xffffff==0x3737f3:red_cross_lines.append(color)
    elif (name.startswith("ImDrawList_") or name in ("forge_log","forge_fs_unmountSaveData","forge_input_setMouseFree","close","destroy")
          or "CloseFile" in name or name=="menu_original"):pass
    else:raise AssertionError(name)
    cpu.reg_write(UC_ARM_REG_R0,result)
    cpu.reg_write(UC_ARM_REG_PC,cpu.reg_read(UC_ARM_REG_LR))


cpu.hook_add(UC_HOOK_CODE,callback,begin=STUB,end=STUB+0xffff)
for addr in tuple(calls):
    if MAIN<=addr<MAIN+0x2000000:cpu.hook_add(UC_HOOK_CODE,callback,begin=addr,end=addr)


def frame(buttons=0, delay=16, finger=None, stick=(0,0)):
    global now, held, touch, left_stick
    now+=delay
    held,touch,left_stick=buttons,finger,stick
    put(IO+24,"<f",delay/1000)
    call(symbols["forge_onUpdate"])


def tap(x,y):
    frame(finger=(x,y));frame()


def state():
    return read(symbols["wheel"],"<II") # visible, editing


def game_pads(expected):
    for detour in pad_hooks:
        call(detour,PAD,PAD+0x80,stop=MAIN+0x1000)
        assert read(PAD+8,"<Qiiii")==expected


def rebound_actions(blocked):
    if not port:
        return
    # The host Actions layer supplies these logical bits independently of the
    # Npad result. In particular 0x20 is CrossRight and 0x4000 is Cancel.
    assert set(action_hooks)=={"gameplay","menu"}
    for view,hook in action_hooks.items():
        obj=HEAP+(0x8a00 if view=="gameplay" else 0x8b00)
        for word in (0x20,0x4000,0x156020):
            action_calls.clear()
            call(hook,obj,word)
            got=cpu.reg_read(UC_ARM_REG_R0)
            if blocked:
                assert got==0 and not action_calls,(view,word,got)
            else:
                assert action_calls==[(view+"_translator",obj,word)]
                assert got==word|(0x1000000 if view=="gameplay" else 0x2000000)


def town_shortcuts(blocked):
    if not port or quest:
        return
    # Even after both translators ran, the host temporarily supplies an
    # additional village-specific (current, previous, pressed, released) view.
    # X opens the world map and Y opens Hunter Search. Preserve the exact view
    # after the original dispatch returns so the host can finish its update.
    hook=menu_hooks[0x792728]
    actor,pad=HEAP+0x8c00,HEAP+0x51000
    previous,=read(MAIN+0x188c670,"<I")
    put(MAIN+0x188c670,"<I",pad)
    for current,old in ((0x1000,0x8000),(0x8000,0x1000),(0,0x1000)):
        words=(current,old,current&~old,old&~current)
        put(pad+0x6bf4,"<4I",*words)
        town_observed.clear()
        call(hook,actor)
        assert town_observed==[(actor,(0,0,0,0) if blocked else words)]
        assert read(pad+0x6bf4,"<4I")==words and cpu.reg_read(UC_ARM_REG_R0)==0x739
    put(MAIN+0x188c670,"<I",0)
    town_observed.clear()
    call(hook,actor)
    assert town_observed==[(actor,None)] and cpu.reg_read(UC_ARM_REG_R0)==0x739
    put(MAIN+0x188c670,"<I",previous)


def render_text(expect_images=True):
    texts.clear()
    images.clear()
    # First use copies each packed page to the port's guest buffer.
    call(symbols["forge_onImGuiFreeRender"],limit=12000000)
    if expect_images:assert images
    return set(texts)


def open_stock_page_three(settle=True):
    menu, rows, hints, pad = [HEAP + n for n in (0x20000, 0x22000, 0x23000, 0x30000)]
    assert state() == (0, 0)
    put(MAIN + 0x188c670, "<I", pad)
    table, per_page, count = (0x177aeb8, 7, 15) if quest else (0x177e424, 8, 17)
    put(menu, "<I", MAIN + table)
    put(menu + 0xa8, "<II", rows, count)
    put(menu + 0xc0, "<II", hints, count)
    put(menu + 0xe5, "<BB", 0, per_page)
    put(menu + 0xe8, "<HHH", 0, 2, count)
    call(menu_hooks[0x66fecc], menu)
    assert read(menu + 0xea, "<H")[0] == 3
    put(menu + 0xe8, "<H", 2)
    put(pad + 0x6bfc, "<I", 0x2000)
    call(menu_hooks[0x66f868], menu)
    assert cpu.reg_read(UC_ARM_REG_R0) == 4
    call(menu_hooks[0x66ff3c], menu)
    put(pad + 0x6bfc, "<I", 0)
    frame(1)
    frame()
    if settle and port:
        frame(delay=100)
    assert state() == (1, 1) and not read(symbols["wheel"] + 8, "<I")[0]


def stock_pad_regression():
    global quest, right_stick
    a, b, y = 1, 2, 8
    left, up, right, down = (1 << i for i in range(12, 16))

    def press(key):
        frame(key)
        frame()

    def focus_details():
        assert var("preset_row") == 0
        press(up)
        assert var("preset_row") == 5
        press(up)
        assert var("preset_row") == 4
        press(right)
        assert var("ui_focus") == 1

    open_stock_page_three()
    focus_details()
    press(up)
    assert var("detail_index") == 11
    press(a)
    assert var("touch_mode", "<B") and var("detail_index") == 0
    press(down)
    assert var("detail_index") == 1, "stock CLOUD ignored the pad after enabling touch mode"
    press(a)
    assert var("position_editing", "<B")
    initial = var("position_x")
    frame(stick=(-1, 0), delay=80)
    assert var("draft_x", "<f") < initial, "position editor ignored the left stick"
    press(b)
    assert not var("position_editing", "<B") and var("position_x") == initial
    press(a)
    frame(stick=(-1, 0), delay=80)
    press(y)
    assert var("draft_x", "<f") == initial
    frame(stick=(-1, 0), delay=80)
    press(a)
    assert not var("position_editing", "<B") and var("position_x") < initial
    for index in (2, 5, 7, 8, 11, 0):
        press(down)
        assert var("detail_index") == index, ("focus moved to a hidden setting", index)
    # CLOUD's stock touch overlay exposes a left analog stick even when its
    # layout has no directional buttons. One deflection moves one row.
    frame(stick=(0, -1))
    frame(stick=(0, -1))
    frame(stick=(0, -.5))
    frame(stick=(0, -1))
    assert var("detail_index") == 1
    frame()
    frame(stick=(0, -1))
    assert var("detail_index") == 2
    frame()
    frame(stick=(-1, 0))
    assert var("ui_focus") == 0
    frame()
    frame(stick=(0, -1))
    frame()
    assert var("preset_row") == 5
    frame(stick=(0,-1))
    frame()
    assert var("preset_row") == 0
    put(HEAP + 0xa01a, "<4H", 10, 2, 12, 1)
    right_stick = (1, -1)
    frame()
    right_stick = (0, 0)
    press(a)
    assert read(symbols["wheel"] + 8, "<III") == (1, 5, 0)
    frame(stick=(0, -1))
    frame(stick=(0, -1))
    assert read(symbols["wheel"] + 16, "<I")[0] == 1
    frame()
    frame(stick=(0, 1))
    assert read(symbols["wheel"] + 16, "<I")[0] == 0
    frame()
    press(down)
    press(a)
    assert read(symbols["wheel"] + 8, "<I")[0] == 0
    assert read(symbols["wheel"] + 20 + 4 * 5, "<I")[0] == 12
    frame(b)
    assert state() == (0, 0)
    rebound_actions(True)
    frame()
    rebound_actions(False)

    # Reload through game storage, as on a restart with the 0.39 touch journal.
    put(symbols["loaded"], "<B", 0)
    put(symbols["touch_mode"], "<B", 0)
    frame()
    assert var("touch_mode", "<B")
    press(64 | 16)
    assert state() == (0, 0)
    quest = False
    put(HEAP + 0xc01c, "<I", 2)
    frame()
    press(64 | 8)
    assert state() == (0, 0)
    quest = True
    frame()
    open_stock_page_three()
    focus_details()
    press(up)
    assert var("detail_index") == 11
    press(a)
    assert not var("touch_mode", "<B")
    press(b)
    assert state() == (0, 0)
    print("PASS stock CLOUD: page 3, six touch settings, pad and virtual stick navigation, position and picker, B close, journal reload and removed shortcuts")


def stock_gear_regression():
    global host_panel, host_finger, host_click_button, host_focused, host_pointer_position
    # Portable can pause guest updates while the Forge settings modal keeps
    # rendering. Exercise a complete UI interaction without a guest update.
    open_stock_page_three()
    put(HEAP + 0xa01a, "<4H", 10, 2, 12, 1)
    put(IO + 8, "<ff", 2400, 1080)
    host_panel = True

    def render_only(point=None):
        global host_finger
        host_finger = None if point is None else (240 + point[0] * 1.5, point[1] * 1.5)
        call(symbols["forge_onImGuiFreeRender"], limit=12000000)

    def click(x, y):
        render_only((x, y))
        render_only()


    render_only()
    assert host_popup and state() == (1, 1)
    click(145, 450)
    assert var("preset_row") == 4 and var("ui_focus") == 1
    render_only((860, 168))
    host_pointer_position = (-10000, -10000)  # Android ACTION_CANCEL
    render_only()
    assert not var("touch_mode", "<B"), "Android cancelled a pointer but it toggled the setting"
    render_only((860, 168))
    host_pointer_position = (240 + 500 * 1.5, 282 * 1.5)
    render_only()
    assert not var("touch_mode", "<B"), "release over another row toggled the pressed setting"
    render_only((860, 168))
    host_focused = False
    render_only()
    host_focused = True
    render_only((860, 168))
    render_only()
    assert not var("touch_mode", "<B"), "focus loss committed an abandoned tap"
    click(860, 168)
    assert var("touch_mode", "<B")
    click(500, 282)
    assert var("position_editing", "<B")
    render_only((var("draft_x", "<f"), var("draft_y", "<f")))
    render_only((480, 390))
    render_only()
    click(654, 47)
    assert not var("position_editing", "<B")
    assert (var("position_x"), var("position_y")) == (480, 390)
    click(500, 282)
    render_only((480, 390))
    render_only((580, 410))
    host_focused = False
    render_only()
    host_focused = True
    render_only((760, 300))
    render_only()
    assert (var("draft_x", "<f"), var("draft_y", "<f")) == (480, 390)
    click(778, 47)
    assert not var("position_editing", "<B")
    assert (var("position_x"), var("position_y")) == (480, 390)
    # Closing the gear keeps an editor launched from the native third page.
    render_only((860, 168))
    host_panel = False
    render_only()
    frame()
    assert not host_popup and state() == (1, 1) and var("touch_mode", "<B")
    host_panel = True
    render_only()
    click(156, 193)
    click(640, 235)
    assert read(symbols["wheel"] + 8, "<III") == (1, 0, 0)
    click(198, 420)
    assert read(symbols["wheel"] + 16, "<I")[0] == 1
    click(1010, 426)
    assert read(symbols["wheel"] + 8, "<I")[0] == 0
    click(156, 570)
    assert state() == (0, 0)
    render_only()
    assert not host_popup

    host_click_button=True
    call(symbols["forge_onImGuiRender"])
    host_click_button=False
    assert state()==(0,0)


call(symbols["forge_onInit"])
call(symbols["forge_onImGuiInit"],context,0,0)
call(symbols["forge_onImGuiInit"],context,0,0)
assert len(font_loads)==int(args.font_mode=="present") and not pages and not cache_sizes
assert len(pad_hooks)==3
if args.preset_chord:
    quest=True
    frame()
    put(MAIN+0x54f260,"<I",0xe12fff1e)  # success chime
    put(symbols["presets"]+8,"<II",15,0)
    def active(): return read(symbols["presets"]+12,"<I")[0]
    def bind(opener,secondary,enabled=1):
        call(symbols["save_bindings"],0,enabled,secondary,opener)
        assert (var("open_key"),var("quick_key"),var("quick_enabled"))==(opener,secondary,enabled)
    bind(0,1)
    frame(64);frame(66)
    assert active()==0
    game_pads((66|0xf00000,111,222,333,-444))
    frame();frame(1);frame(3)
    assert active()==1
    game_pads((1|0xf00000,111,222,333,-444))
    frame(3);frame(3);assert active()==1
    frame(1);frame(3);assert active()==2
    frame();bind(0,1,0);frame(3)
    assert active()==2
    game_pads((3|0xf00000,111,222,333,-444))
    frame();bind(6,1);frame(66);assert active()==3
    frame();bind(0,1);quest=False;frame(3);assert active()==3
    frame()
    open_stock_page_three()
    put(symbols["preset_row"],"<I",4)
    put(symbols["ui_focus"],"<I",1)
    artwork=[]
    def record_icon(vm,address,size,data):
        artwork.append(vm.reg_read(UC_ARM_REG_R3))
    hook=cpu.hook_add(UC_HOOK_CODE,record_icon,begin=symbols["atlas_icon"],end=symbols["atlas_icon"])
    for opener in (0,8):
        bind(opener,1);artwork.clear()
        lines=render_text()
        assert "游戏中可以按" in lines and "+自定义键切换预设" in lines,lines
        if opener==0: initial=artwork[:]
        else:
            assert len(initial)==len(artwork)
            assert [(a,b) for a,b in zip(initial,artwork) if a!=b]==[(0,8),(0,8)],(initial,artwork)
    cpu.hook_del(hook)
    print("PASS configured opener + secondary, original chord inactive, repeat, disable, input masking and live menu artwork")
    raise SystemExit(0)
if args.settings_entry:
    assert port
    for in_quest in (False,True):
        quest=in_quest
        for source in ("right mouse", "Escape", "left mouse", "Return", "delayed B"):
            print("Testing",source,"quest=",in_quest,flush=True)
            frame()
            if source.endswith("mouse"):
                pc_mouse=(640,235,4 if source=="right mouse" else 1)
            if source=="Escape":pc_keys.add(41)
            if source=="Return":pc_keys.add(40)
            open_stock_page_three(settle=False)
            frame()
            if source=="delayed B":
                frame(2); frame(2,180)
            else:
                frame(delay=180)
            assert state()==(1,1) and not read(symbols["wheel"]+8,"<I")[0],source
            pc_mouse=(640,235,0);pc_keys.clear()
            frame();frame()
            assert "快捷轮盘设置" in render_text()
            pc_mouse=(640,235,1);frame()
            assert read(symbols["wheel"]+8,"<I")[0],source
            pc_mouse=(640,235,0);frame()
            pc_keys.add(41);frame();pc_keys.clear();frame()
            assert state()==(1,1) and not read(symbols["wheel"]+8,"<I")[0]
            pc_mouse=(640,235,4);frame()
            assert state()==(0,0)
            pc_mouse=None;frame()
    assert not cursor_requests
    print("PASS editor entry, held/late input, fresh select and Back, village and quest")
    raise SystemExit(0)
if args.prowler_play:
    from test_prowler_play import verify_prowler_play
    verify_prowler_play(globals())
    raise SystemExit(0)
if args.prowler_settings:
    from test_prowler_settings import verify_prowler_settings
    verify_prowler_settings(globals())
    raise SystemExit(0)
if args.scale_only:
    from test_scale import verify_scale
    verify_scale(globals())
    raise SystemExit(0)
if args.stock_cloud:
    quest = True
    frame()
    if args.stock_cloud in ("both", "pad"):
        stock_pad_regression()
    if args.stock_cloud in ("both", "gear"):
        stock_gear_regression()
    print("PASS stock CLOUD NRO", sha256(image).hexdigest())
    raise SystemExit(0)
frame();assert state()==(0,0)
rebound_actions(False)
town_shortcuts(False)
frame(72);assert state()==(0,0)
frame();open_stock_page_three()
game_pads((0,0,0,0,0))
rebound_actions(True)
town_shortcuts(True)
frame()
tap(90,450);assert var("preset_row")==4 and var("ui_focus")==1
tap(500,540);assert var("mapping","<B")
frame();frame(2);assert var("open_key")==1 and state()==(1,1)
frame(2);assert state()==(1,1)
frame()
assert len([p for p in files if "settings_" in p])==1
assert {"自定义呼出轮盘按键", "不透明度"} <= render_text()
put(symbols["english"], "<I", 1)
assert "Opacity" in render_text()
put(symbols["english"], "<I", 0)
assert pages
if args.font_mode=="present":assert cache_sizes==[24.0,36.0] and read(face+8,"<I")[0]&8
else:assert not cache_sizes and not read(default_face+8,"<I")[0]&8
tap(475,233);assert var("position_editing","<B")
editor=render_text()
assert "调整轮盘位置" not in editor and "左摇杆移动，或触屏拖动轮盘" in editor
frame(stick=(-1,0),delay=80);x=var("draft_x","<f");assert x<1113
frame(finger=(x,360));frame(finger=(700,420));frame()
assert abs(var("draft_x","<f")-700)<1 and abs(var("draft_y","<f")-420)<1
frame(1);assert not var("position_editing","<B") and (var("position_x"),var("position_y"))==(700,420)
frame();tap(475,233);frame(8);assert (var("draft_x","<f"),var("draft_y","<f"))==(1113,360)
frame();frame(2);assert state()==(1,1) and (var("position_x"),var("position_y"))==(700,420)
frame();frame(2);assert state()==(0,0)
rebound_actions(True)
town_shortcuts(True)
frame(2);rebound_actions(True);town_shortcuts(True)
frame();rebound_actions(False);town_shortcuts(False)
quest=True;frame(2,230);assert state()==(0,0)
frame();frame(64);frame(64,220);assert state()==(0,0)
frame();frame(2);frame(2,190);assert state()==(0,0)
game_pads((2|0xf00000,111,222,333,-444))
frame(2,14);assert state()==(1,0)
game_pads((2,111,222,0,0))
rebound_actions(False)
frame();assert state()==(0,0)
game_pads((0xf00000,111,222,333,-444))
frame(80);assert state()==(0,0)
frame();open_stock_page_three()
game_pads((0,0,0,0,0))
if "plus_input" in symbols:
    # Replay the native page selection into the plugin callback. The native
    # menu-specific fixture separately covers initial allocation and pagination.
    frame();frame(2);frame()
    menu,rows,hints,pad=[HEAP+n for n in (0x20000,0x22000,0x23000,0x30000)]
    put(MAIN+0x188c670,"<I",pad)
    menus=[(True,0x177aeb8,7,15,"synchronized"),(False,0x177e424,8,17,"synchronized")]
    if port:
        menus.extend((True,0x177aeb8,7,15,snapshot) for snapshot in ("late_A","late_B","neutral"))
    for in_quest,table,per_page,count,snapshot in menus:
        quest=in_quest
        frame()
        put(menu,"<I",MAIN+table)
        put(menu+0xa8,"<II",rows,count);put(menu+0xc0,"<II",hints,count)
        put(menu+0xe5,"<BB",0,per_page);put(menu+0xe8,"<HHH",0,2,count)
        call(menu_hooks[0x66fecc],menu)
        assert read(menu+0xea,"<H")[0]==3
        for language, expected in enumerate(['ショートカットの設定', 'Configure Shortcut Wheel', 'Configurer les raccourcis', 'Configurar accesos directos', 'Kurzmenü einrichten', 'Configura menu rapido', '바로 가기 설정', '配置快捷輪盤', '配置快捷轮盘', 'Configure Shortcut Wheel']):
            put(HEAP+0xc000+0xa5,"<B",language)
            call(menu_hooks[0x66f868],menu)
            assert string(read(rows+(count-1)*8+4,"<I")[0])==expected
        frame(2);frame(2,240);assert state()==(0,0)
        put(menu+0xe8,"<H",2);put(pad+0x6bfc,"<I",0x2000)
        call(menu_hooks[0x66f868],menu)
        assert cpu.reg_read(UC_ARM_REG_R0)==4
        call(menu_hooks[0x66ff3c],menu)
        # The game's native Confirm and Forge's handheld input snapshot can
        # reach different callbacks. A neutral snapshot at handoff must not
        # allow the same physical press to enter the wheel's item picker.
        frame(1 if snapshot=="synchronized" else 0)
        assert state()==(1,1) and not read(symbols["wheel"]+8,"<I")[0]
        rebound_actions(True)
        town_shortcuts(True)
        next_buttons={"synchronized":1,"late_A":1,"late_B":2,"neutral":0}[snapshot]
        frame(next_buttons)
        assert state()==(1,1) and not read(symbols["wheel"]+8,"<I")[0],snapshot
        if snapshot!="synchronized":
            frame(next_buttons)
            game_pads((0,0,0,0,0))
            assert "从道具袋选择目标道具" not in render_text()
            put(pad+0x6bfc,"<I",0)
            frame(delay=100)
            if snapshot!="late_B":
                frame(1)
                assert state()==(1,1) and read(symbols["wheel"]+8,"<I")[0]
                assert "从道具袋选择目标道具" in render_text()
                frame();frame(2);frame()
                assert state()==(1,1) and not read(symbols["wheel"]+8,"<I")[0]
        frame(delay=100);frame(2);frame()
        assert state()==(0,0)
    print("PASS native village and quest page 3 Confirm -> same settings, no input leak")
    if port:print("PASS port handoff: late A/B and neutral snapshots, fresh confirm/cancel and picker text")
    if port:print("PASS remapped gameplay and menu inputs blocked in settings and through B release, both translators restored")
    if port:print("PASS village X/Y: private action snapshot blocked during original dispatch and restored intact, native updates preserved")
if "touch_mode" in symbols:
    quest=True
    frame();open_stock_page_three();frame()
    assert state()==(1,1) and not var("touch_mode","<B")
    # The APK's controller mode accepts fingers throughout the editor.
    # Previewing a controller preset must not change the live loadout.
    active=read(symbols["presets"]+12,"<I")[0]
    other=(active+1)%4
    if args.portable_bridge:
        frame(finger=(150,194+other*64))
        bridge_lift=(990,601)
        frame();bridge_lift=None
        assert var("preset_row")==active, "lifting outside the preset committed a tap"
    for index,item in ((0,10),(1,11),(16,12)):
        put(HEAP+0xa01a+4*index,"<HH",item,2)
    tap(150,194+other*64)
    assert var("preset_row")==other and var("previewing","<B")
    assert read(symbols["presets"]+12,"<I")[0]==active
    for slot,(x,y) in enumerate(((640,235),(750,345),(640,455),(530,345),(718,267),(718,423),(562,423),(562,267))):
        tap(x,y)
        assert read(symbols["wheel"]+8,"<II")== (1,slot)
        if slot==0:
            assert {"清除此方向","上下选择，左右翻页"} <= render_text(False)
            tap(258,360);tap(138,360)
            tap(500,200)
            assert read(symbols["wheel"]+16,"<I")[0]==1
        tap(1090,324)
        assert read(symbols["preview_targets"]+4*slot,"<I")[0]==(11 if slot==0 else 10)
    tap(640,235);tap(986,530)
    assert read(symbols["preview_targets"],"<I")[0]==0
    tap(640,235);tap(1010,426)
    assert not read(symbols["wheel"]+8,"<I")[0]
    tap(609,175);tap(754,172)
    assert read(symbols["presets"]+12,"<I")[0]==other
    tap(150,194+active*64);tap(754,172)
    assert read(symbols["presets"]+12,"<I")[0]==active
    tap(151,510);old=var("cat_opacity")
    tap(500,389);assert var("cat_opacity")!=(old)
    for _ in range(3): tap(500,389)
    assert var("cat_opacity")==old
    tap(135,449)
    tap(500,440);assert var("info_open","<B")
    tap(900,502);assert not var("info_open","<B")
    tap(500,407);assert var("help_open","<B")
    tap(900,484);assert not var("help_open","<B")
    tap(500,540);tap(900,488);assert not var("mapping","<B")
    original=var("open_key")
    for key in (7,original):
        tap(500,540);tap(347+82*(key%8),353+62*(key//8))
        assert not var("mapping","<B") and var("open_key")==key
    assert not var("touch_mode","<B")
    tap(155,570);assert state()==(0,0)
    frame();open_stock_page_three();frame()
    tap(135,449)
    assert {"切换操作模式","手柄模式","自定义呼出轮盘按键"} <= render_text()
    tap(860,168)
    assert var("touch_mode","<B") and var("preset_row")==4
    details=render_text(False)
    assert {"触屏模式","轮盘模式","调整轮盘位置","语言/Language","触发方式","说明","不透明度"}<=details
    assert not {"自定义呼出轮盘按键","设置按键图标风格","快捷切换","当前道具组"}&details
    tap(600,452);assert var("info_open","<B")
    tap(901,508);assert not var("info_open","<B")
    tap(500,282);assert var("position_editing","<B")
    x,y=var("draft_x","<f"),var("draft_y","<f")
    frame(finger=(x,y));frame(finger=(480,390));frame()
    tap(654,47)
    assert (var("position_x"),var("position_y"))==(480,390)
    tap(510,282);tap(912,49)
    assert (var("draft_x","<f"),var("draft_y","<f"))==(1113.,360.)
    tap(778,670);assert (var("position_x"),var("position_y"))==(480,390)
    tap(147,322);assert var("preset_row")==2 and not var("previewing","<B")
    # Set all eight slots using only the side panel, the slot, and picker controls.
    for index,item in ((0,10),(1,11),(16,12)):
        put(HEAP+0xa01a+4*index,"<HH",item,2)
    for slot,(x,y) in enumerate(((640,235),(750,345),(640,455),(530,345),(718,267),(718,423),(562,423),(562,267))):
        tap(x,y)
        assert read(symbols["wheel"]+8,"<III")== (1,slot,0)
        if slot == 0:
            # Android can publish the same touch both as coordinates and as
            # an overlaid virtual gamepad button. One gesture moves one item.
            frame(1 << 15, finger=(198, 420))
            frame(1 << 15)
            frame()
        else:
            tap(198,420)
        assert var("wheel")==1 and read(symbols["wheel"]+16,"<I")[0]==1
        tap(258,360);assert read(symbols["wheel"]+16,"<I")[0]==16
        if slot==0:
            assert "上下选择，左右翻页" in render_text(False)
            tap(138,360);assert read(symbols["wheel"]+16,"<I")[0]==0
            tap(198,300);assert read(symbols["wheel"]+16,"<I")[0]==16
            tap(1010,426);assert read(symbols["wheel"]+8,"<I")[0]==0
            tap(x,y);tap(258,360)
        tap(1090,324)
        assert read(symbols["wheel"]+8,"<I")[0]==0
        assert read(symbols["wheel"]+20+4*slot,"<I")[0]==12
    tap(155,198);assert read(symbols["wheel"]+20,"<8I")== (0,)*8
    tap(154,324);assert read(symbols["wheel"]+20,"<8I")== (12,)*8
    assert "当前使用" in render_text()
    tap(155,570);assert state()==(0,0)
    # Reload through the normal callback and verify the saved mode and active slots.
    put(symbols["loaded"],"<B",0);put(symbols["touch_mode"],"<B",0)
    frame();assert var("touch_mode","<B") and read(symbols["wheel"]+20,"<8I")== (12,)*8
    if args.portable_bridge:
        assert bridge_region==(1,480.,390.,36.)
        frame(finger=(480,390))
        bridge_cancel=True;frame();bridge_cancel=False;frame()
        assert state()==(0,0)
    tap(480,390);assert state()==(0,0)
    tap(480,390);assert state()==(0,0)
    open_stock_page_three()
    if args.portable_bridge:
        frame();assert bridge_region[0]==2
        frame(finger=(145,450))
        bridge_cancel=True;frame();bridge_cancel=False;frame()
        assert var("preset_row")!=4
    tap(152,450);tap(864,165);assert not var("touch_mode","<B")
    print("PASS Android: touch/controller modes, six settings, tap all slots, d-pad/confirm/back, preset reload, drag/save/cancel/reset")
    # Portable 0.2 deliberately stops publishing forge_input_getTouch with its
    # own panel open. Only the ImGui mouse (or a forwarded Android finger) moves.
    frame();frame(2);frame()
    assert state()==(0,0)
    put(IO+8,"<ff",2400,1080)
    host_panel=True;host_click_button=True
    call(symbols["forge_onImGuiRender"])
    assert state()==(0,0)
    if args.portable_bridge:assert {area[0] for area in bridge_seen}=={0,1,2}
print("PASS wheel callbacks")
