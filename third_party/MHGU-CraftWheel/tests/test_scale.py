"""Run from the ARM callback harness with --scale-only and --portable-bridge."""
import struct
from zlib import crc32


def record(*words):
    payload = struct.pack("<" + "I" * len(words), *words)
    # The game's save helper stores the uncomplemented CRC accumulator.
    return payload + struct.pack("<I", crc32(payload) ^ 0xffffffff)


def verify_scale(env):
    assert env["args"].portable_bridge
    files, var, put = env["files"], env["var"], env["put"]
    frame, tap, render = env["frame"], env["tap"], env["render_text"]
    a_path = "savedata:/gu_wheel_position_a.bin"
    b_path = "savedata:/gu_wheel_position_b.bin"

    def saved():
        return tuple(var(key) for key in ("position_x", "position_y", "position_percent"))

    def reload():
        put(env["symbols"]["loaded"], "<B", 0)
        frame()

    def edit():
        tap(475, 233)
        assert var("position_editing", "<B")

    def draft():
        return var("draft_x", "<f"), var("draft_y", "<f"), var("draft_percent")

    # The offset and sequence of the old side-based and free-position records
    # differ. Load each before writing the first record of the new format.
    files[a_path] = record(0x31505747, 6, 1)
    frame()
    assert saved() == (167, 560, 100)
    files[b_path] = record(0x32505747, 7, 1152, 360)
    reload()
    assert saved() == (1152, 360, 100)
    env["open_stock_page_three"]()
    frame()
    tap(90, 450)
    edit()

    # Sample the transform while the shipped NRO actually draws its sprites.
    # The controller symbols below the wheel use the original screen scale.
    class Samples(list):
        def append(self, handle):
            self.transforms.add(tuple(var(key, "<f") for key in ("scale", "origin_x", "origin_y")))
            super().append(handle)

    samples = Samples()
    env["images"] = samples
    put(env["IO"] + 8, "<ff", 2400, 1080)
    fit, letterbox = 1.5, 240

    def assert_rendered(percent, x, y, editing):
        samples.transforms = set()
        text = render()
        if editing:
            assert "轮盘大小" in text and f"{percent}%" in text
            assert (fit, letterbox, 0) in samples.transforms
        zoom = fit * percent / 100
        expected = (zoom, letterbox + x * (fit - zoom), y * (fit - zoom))
        assert any(all(abs(a-b) < .015 for a,b in zip(actual, expected))
                   for actual in samples.transforms), (samples.transforms, expected)

    # Drag through every point of the scale. A growing wheel near an edge is
    # moved in far enough for its enlarged outer frame and quantity labels.
    frame(finger=(505, 607))
    for percent in range(50, 151):
        frame(finger=(505 + (percent-50)*3.95, 607))
        assert var("draft_percent") == percent
    frame()
    assert draft() == (1088, 360, 150)
    assert_rendered(150, 1088, 360, True)
    assert saved() == (1152, 360, 100)
    frame(1)
    frame()
    assert saved() == (1088, 360, 150)
    assert files[a_path] == record(0x33505747, 8, 1088, 360, 150)
    reload()
    assert saved() == (1088, 360, 150)

    # Canceled Android touch restores both the zoom and the center, including
    # any automatic adjustment made while the scale was changing.
    edit()
    tap(505, 607)
    assert draft() == (1088, 360, 50)
    assert_rendered(50, 1088, 360, True)
    frame(finger=(1088, 360))
    frame(finger=(1152, 360))
    frame()
    frame(finger=(900, 607))
    assert draft() == (1088, 360, 150)
    env["bridge_cancel"] = True
    frame()
    env["bridge_cancel"] = False
    frame()
    assert draft() == (1152, 360, 50)
    frame(2)
    frame()
    assert saved() == (1088, 360, 150)

    # Y and A/B act on position and scale as one edit. D-pad repeats after
    # a hold delay and clamps to both limits even after passing an endpoint.
    edit()
    frame(8)
    frame()
    assert draft() == (1113, 360, 100)
    left, right = 1 << 12, 1 << 14
    frame(left)
    assert var("draft_percent") == 99
    frame(left, delay=200)
    assert var("draft_percent") == 99
    for _ in range(56):
        frame(left, delay=80)
    assert var("draft_percent") == 50
    frame()
    frame(right)
    assert var("draft_percent") == 51
    frame()
    tap(900, 607)
    frame(8)
    frame()
    frame(2)
    frame()
    assert saved() == (1088, 360, 150)

    # The stock Forge panel may run several ImGui frames without an Update.
    # Move the slider only in the lift event and verify focus-loss rollback.
    edit()
    env["host_panel"] = True
    render()
    def modal_tick(finger=None):
        env["host_finger"] = None if finger is None else (letterbox + finger[0]*fit, finger[1]*fit)
        render()
    modal_tick()
    modal_tick((505, 607))
    assert draft()[2] == 50
    env["host_pointer_position"] = (letterbox + (505 + 3.95*75)*fit, 607*fit)
    modal_tick()
    assert draft()[2] == 125
    modal_tick((900, 607))
    assert draft()[2] == 150
    env["host_focused"] = False
    modal_tick()
    env["host_focused"] = True
    modal_tick()
    assert draft()[2] == 125
    modal_tick((775, 670))
    modal_tick()
    assert not var("position_editing", "<B") and saved()[2] == 150
    env["host_panel"] = False
    render()

    # Open a real mission wheel without touching any internal render flag.
    frame(2)
    frame()
    env["quest"] = True
    frame()
    frame(64)
    frame(64, delay=220)
    frame(64, delay=160)
    assert env["state"]() == (1, 0)
    assert_rendered(150, 1088, 360, False)
    frame()

    # Damaged and out-of-range records must fall back to the older valid slot.
    files[a_path] = record(0x33505747, 8, 1088, 360, 151)
    reload()
    assert saved() == (1152, 360, 100)
    files[a_path] = record(0x33505747, 8, 1152, 360, 150)
    reload()
    assert saved() == (1152, 360, 100)
    files[a_path] = record(0x33505747, 8, 1000, 360, 133)[:-2]
    reload()
    assert saved() == (1152, 360, 100)
    print("PASS scale: both prior layouts, bridge and pad, 101 sizes, edge clamping, "
          "cancel/reset/save/reload, 20:9 preview and mission drawing, interrupted writes")
