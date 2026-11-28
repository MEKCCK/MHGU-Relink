"""Run via test_callback.py --prowler-settings; Android also supports --portable-bridge."""
def verify_prowler_settings(env):
    frame, tap, var, put = (env[k] for k in ("frame", "tap", "var", "put"))
    symbols, files = env["symbols"], env["files"]
    android = "touch_mode" in symbols
    up, left, right, down = 1 << 13, 1 << 12, 1 << 14, 1 << 15

    def press(button):
        frame(button)
        frame()

    def cat():
        return tuple(var(name) for name in ("cat_enabled", "cat_trigger", "cat_opacity", "cat_x", "cat_y", "cat_scale"))

    def hunter():
        names = ["release_trigger", "opacity", "position_x", "position_y"]
        if android:
            names.append("position_percent")
        return tuple(var(name) for name in names)

    def reload():
        put(symbols["loaded"], "<B", 0)
        frame()

    frame()
    initial = hunter()
    assert cat() == (1, 0, 3, 1113, 360, 100)
    press(72)
    assert env["state"]()==(0,0)
    env["open_stock_page_three"]()
    tap(90,450)
    assert var("preset_row")==4
    assert "自动纳刀" not in env["render_text"](False)
    press(up)
    assert var("detail_index")== (11 if android else 10)
    if android:
        press(up)
        assert var("detail_index")==10
    tap(600,168)
    assert var("detail_index")==10
    press(2)
    env["open_stock_page_three"]()
    press(up)
    assert var("preset_row") == 5 and not var("ui_focus")
    assert {"猎猫设置", "猎猫轮盘", "不透明度"} <= env["render_text"](False)
    press(right)
    press(1)
    assert not var("cat_enabled")
    press(down)
    press(1)
    assert var("cat_trigger") == 1
    press(left)
    press(up)
    assert var("preset_row") == 4
    if android:
        tap(860, 168)
        assert var("touch_mode", "<B")
    tap(100, 508)
    assert var("preset_row") == 5 and var("ui_focus")
    tap(450, 396)
    assert var("cat_opacity") == 0
    tap(450, 340)
    assert var("position_editing", "<B") and var("cat_position_edit", "<B")
    frame(finger=(1113, 360))
    frame(finger=(600, 425))
    frame()
    assert (var("draft_x", "<f"), var("draft_y", "<f")) == (600., 425.)
    if android:
        tap(505, 111)
        assert var("draft_percent") == 50
        tap(650, 48)
    else:
        press(1)
    expected = (0, 1, 0, 600, 425, 50 if android else 100)
    assert not var("position_editing", "<B") and cat() == expected
    reload()
    assert cat() == expected and hunter() == initial
    tap(100, 508)
    tap(450, 340)
    press(8)
    assert var("draft_x", "<f") == 1113 and var("draft_y", "<f") == 360
    press(2)
    assert cat() == expected and hunter() == initial
    before_edit = {n for n in files if "gu_wheel_cat_" in n}
    assert len(before_edit) == 2
    values = sorted((int.from_bytes(files[n][4:8], "little"), n) for n in before_edit)
    files[values[-1][1]] = files[values[-1][1]][:-1]
    reload()
    assert cat() == (0, 1, 0, 1113, 360, 100) and hunter() == initial
    print("PASS", env["args"].project.name, "Prowler sixth row, controls, touch, separate options/position/size, cancel and journal recovery")
