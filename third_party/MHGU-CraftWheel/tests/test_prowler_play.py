"""Exercise the released NRO with changing equipped moves and native action checks."""
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_LR, UC_ARM_REG_PC


def verify_prowler_play(env):
    cpu, main, heap, sym = (env[k] for k in ("cpu", "MAIN", "HEAP", "symbols"))
    put, read, frame, var, state = (env[k] for k in ("put", "read", "frame", "var", "state"))
    actor, data, manager = heap + 0x72000, heap + 0x81000, heap + 0x90000
    record = data + 4
    local_actor = actor
    allowed = True
    checks, requests = [], []

    def intercept(offset, handler):
        def run(*_):
            result = handler(cpu.reg_read(UC_ARM_REG_R0), cpu.reg_read(UC_ARM_REG_R1))
            cpu.reg_write(UC_ARM_REG_R0, result)
            cpu.reg_write(UC_ARM_REG_PC, cpu.reg_read(UC_ARM_REG_LR))
        address = main + offset
        cpu.hook_add(UC_HOOK_CODE, run, begin=address, end=address)

    def submit(owner, action):
        assert owner == actor
        requests.append(action)
        return 0

    intercept(0x277334, lambda *_: local_actor)
    intercept(0x264e7c, lambda _, action: action)
    intercept(0x550ecc, lambda action, _: checks.append(action) or int(not allowed))
    intercept(0x2b3118, submit)
    for offset, signature in ((0x65a1c4, 0xebea3a65), (0x654750, 0xebfbf1dd), (0x6547c4, 0xeaf17a53)):
        put(main + offset, "<I", signature)
    put(main + 0x1885b58, "<I", manager)
    put(main + 0x1885468, "<I", manager)
    energy = 3000
    cost = {31:2000, 2:1000, 56:5000}
    intercept(0xe8ce4,lambda _, action: cost.get(action,0))
    intercept(0x2a1110,lambda owner, required: int(owner==actor and energy>=required))
    put(main+0x1218998,"<I",0xebbb40d1)
    put(main+0x12189a4,"<I",0xe5902470)
    put(actor,"<I",heap+0x71000)
    put(heap+0x71000+0x470,"<I",main+0x2a1110)
    put(manager+0x2b3c,"<I",manager)
    put(actor + 0x13c4, "<I", data)
    eight = (31, 2, 42, 4, 5, 56, 20, 8)
    put(record + 0x244, "<8B", *eight)
    put(record + 0x274, "<Q", 0xaba123)
    env["quest"] = True
    frame()
    assert var("playing_prowler")
    assert read(sym["cat_actions"], "<8I") == eight
    original = (93, 9, 8, 37, 121, 19, 221, 130)
    put(sym["wheel"] + 20, "<8I", *original)
    # Distinct hunter trigger and opacity must not affect the cat.
    put(sym["release_trigger"], "<I", 2)
    key = 1 << var("open_key")
    axes = ((0,1),(1,0),(0,-1),(-1,0),(1,1),(1,-1),(-1,-1),(-1,1))

    def stick(x=0,y=0,buttons=key,delay=16):
        env["right_stick"] = (x,y)
        frame(buttons,delay)

    def check_ring(percent):
        env["image_geometry"].clear()
        env["wheel_rings"].clear()
        env["render_text"]()
        rings=env["wheel_rings"]
        assert len(rings)==2
        for (cx,cy,r,color,segments,width,prior_sprites),rgb,weight in zip(
                rings,(0,0x5e655d),(6,2)):
            assert segments%8==0 and segments>=32 and prior_sprites==0
            assert color==(63<<24)|rgb and abs(width-weight*percent/100)<.001
            assert abs(r-75*percent/100)<.001
            for x0,y0,x1,y1,_ in env["image_geometry"][:16:2]:
                distance=(((x0+x1)/2-cx)**2+((y0+y1)/2-cy)**2)**.5
                assert abs(distance-r)<.001

    def open_wheel():
        stick(buttons=0)
        stick()
        stick(delay=215)
        assert state() == (1,0)

    put(sym["cat_x"],"<I",640)
    put(sym["cat_y"],"<I",310)
    put(sym["cat_opacity"],"<I",0)
    open_wheel()
    for i,(x,y) in enumerate(axes):
        stick(x,y)
        stick(x,y,delay=160)
        assert requests == list(eight[:i+1])
        assert var("wheel") == 1 and read(sym["wheel"]+12,"<I")[0] == i
        if i==0:
            env["image_geometry"].clear()
            assert "随从鼓舞之技" in env["render_text"]()
            sprites=env["image_geometry"]
            assert len(sprites)==17 and all(sprite[4]==63 for sprite in sprites),sprites
            for i,(x0,y0,x1,y1,_) in enumerate(sprites[:16:2]):
                assert (round(x1-x0),round(y1-y0))==((71,60) if i==0 else (48,48))
            for (x0,y0,x1,y1,_),(ux,uy) in zip(sprites[1:16:2],axes):
                factor=53 if ux and uy else 75
                assert abs((x0+x1)/2-(640+ux*factor))<12
                assert abs((y0+y1)/2-(310-uy*factor))<12
            check_ring(100)
            if env["args"].project.name=="android":
                for percent in (50,150):
                    put(sym["cat_scale"],"<I",percent)
                    check_ring(percent)
                put(sym["cat_scale"],"<I",100)
    stick(buttons=0)
    put(sym["cat_trigger"], "<I", 1)
    put(record+0x244, "<8B", 0,31,0,0,51,4,0,0)
    open_wheel()
    assert read(sym["cat_actions"], "<8I") == (31,51,4,0,0,0,0,0)
    count = len(requests)
    for i,(x,y) in enumerate(axes):
        stick(x,y)
        assert len(requests)==count+min(i,3)
        stick()
        assert len(requests)==count+min(i+1,3)
    assert requests[-3:]==[31,51,4]
    stick(-1,0)
    assert not any("之技" in s for s in env["render_text"]()) and len(env["images"])==12
    stick()
    assert requests[-3:]==[31,51,4]
    # A gauge change must be visible without releasing the wheel. Empty
    # and zero-cost slots have no cross, including at zero energy.
    energy=0
    stick()
    assert var("cat_low_gauge","<B")==1
    env["red_cross_lines"].clear()
    env["render_text"]()
    assert len(env["red_cross_lines"])==2
    energy=2000
    stick()
    assert var("cat_low_gauge","<B")==0
    env["red_cross_lines"].clear()
    env["render_text"]()
    assert not env["red_cross_lines"]
    # Changing cat, even to one with identical moves, cancels a held selection.
    stick(0,1)
    put(record+0x274, "<Q", 1234)
    stick()
    stick(delay=220)
    assert state()==(0,0) and len(requests)==count+3
    open_wheel()
    allowed = False
    stick(1,0)
    stick()
    assert checks[-1]==51 and len(requests)==count+3
    allowed = True
    if "touch_mode" in sym and env["args"].portable_bridge:
        stick(buttons=0)
        put(sym["touch_mode"],"<B",1)
        put(sym["cat_x"],"<I",625)
        put(sym["cat_y"],"<I",300)
        for trigger in (0,1):
            put(sym["cat_trigger"],"<I",trigger)
            frame()
            assert env["bridge_region"]==(1,625.,300.,36.)
            before=len(requests)
            frame(finger=(625,300))
            frame(finger=(625,300),delay=215)
            assert state()==(1,0)
            frame(finger=(690,300))
            assert len(requests)==before+int(trigger==0)
            frame()
            assert requests[-1]==51 and len(requests)==before+1 and state()==(0,0)
        put(sym["touch_mode"],"<B",0)
    put(sym["cat_enabled"], "<I", 0)
    stick(buttons=0)
    stick()
    stick(delay=215)
    assert state()==(0,0)
    if env["args"].project.name=="port":
        put(sym["cat_enabled"],"<I",1)
        put(sym["cat_trigger"],"<I",1)
        open_wheel()
        stick()
        assert env["cursor_requests"][-2:]==[(1,640,310),(2,0,0)]
        warp_count=len(env["cursor_requests"])
        for i,(x,y) in enumerate(axes):
            env["pc_mouse"]=(640+x*75,310-y*75,0)
            stick()
            assert read(sym["wheel"]+12,"<I")[0]==i
        assert len(env["cursor_requests"])==warp_count
        env["pc_mouse"]=(715,310,0)
        stick()
        previous=len(requests)
        stick(buttons=0)
        assert requests[previous:]==[51] and state()==(0,0)
        env["pc_mouse"]=None
    # Reverting to a hunter restores the exact original eight item IDs.
    local_actor=0
    stick(buttons=0)
    assert not var("playing_prowler")
    open_wheel()
    assert read(sym["wheel"]+20,"<8I")==original
    stick(delay=160)
    env["wheel_rings"].clear()
    env["render_text"]()
    assert not env["wheel_rings"]
    # The village resolver must distinguish the local Prowler from a
    # hunter with a Palico in the party, without invoking quest actions.
    intercept(0x2772c4,lambda *_: actor)
    env["quest"]=False
    put(heap+0xc000+0x1c,"<I",2)
    put(actor,"<I",main+0x1748974)
    put(actor+0x13cc,"<B",4)
    frame()
    assert var("playing_prowler") and read(sym["cat_actions"],"<3I")== (31,51,4)
    put(actor,"<I",main+0x1748534)
    frame()
    assert not var("playing_prowler")
    print("PASS native callback routing, eight directions, automatic loadout/order, empty slots,"
          " trigger isolation, held input, disabled wheel, native rejection and hunter restoration")
