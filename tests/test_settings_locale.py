"""Check host UI translation coverage without game files or external packages."""
import ast
from collections import Counter
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
LITERAL = r'"(?:\\.|[^"\\])*"'


def literals(text):
    return [ast.literal_eval(value) for value in re.findall(LITERAL, text)]


def rows(text, columns):
    row = r'\{\s*' + LITERAL + (r'\s*,\s*' + LITERAL) * (columns - 1) + r'\s*\}'
    return [tuple(literals(value)) for value in re.findall(row, text)]


def has_chinese(text):
    return any(chr(0x4e00) <= char <= chr(0x9fff) for char in text)


def default_ini_keys(source):
    """Follow config_help's shared comment blocks, including generated layout keys."""
    source = source.split("static const char DEFAULT_INI[] =", 1)[1].split("void config_path(", 1)[0]
    lines = []
    in_macro = False
    for line in source.splitlines():
        if line.startswith("#define DEFAULT_LAYOUT_COMBOS"):
            in_macro = True
            continue
        if in_macro:
            in_macro = line.rstrip().endswith(chr(92))
            continue
        if line.lstrip().startswith("DEFAULT_LAYOUT_COMBOS("):
            prefix = literals(line)[0]
            for number in range(1, 7):
                lines.extend((f"{prefix}Combo{number} =", f"{prefix}Combo{number}Pad = None"))
        elif not line.startswith("#"):
            lines.extend("".join(literals(line)).splitlines())
    keys, help_keys = set(), set()
    comment = False
    for line in lines:
        if line.startswith(";"):
            comment = True
        elif not line.strip() or line.startswith("["):
            comment = False
        elif "=" in line:
            key = line.split("=", 1)[0].strip()
            keys.add(key)
            if comment:
                help_keys.add(key)
    return keys, help_keys


class SettingsLocaleTest(unittest.TestCase):
    def test_catalog_and_visible_labels(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        triples = [tuple(literals(value)) for value in re.findall(
            r'\{\s*' + LITERAL + r'\s*,\s*' + LITERAL + r'\s*,\s*' + LITERAL + r'\s*\}', catalog)]
        keys = [en for en, _, _ in triples]
        self.assertTrue(triples)
        self.assertFalse([key for key, count in Counter(keys).items() if count > 1])
        self.assertTrue(all(en.strip() and es.strip() and zh.strip() for en, es, zh in triples))
        self.assertNotIn(chr(0xfffd), catalog)
        translations = {en: es for en, es, _ in triples}
        self.assertEqual(translations["Misc"], "Varios")
        self.assertEqual(translations["Hunter"], "Cazador")
        self.assertEqual(translations["Reset this tab"], "Restablecer esta pestaña")

        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        source = re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)
        # PHONE_HELP: the phone's help texts (key, English, Spanish, Chinese) are checked separately: shown in place
        # of the serialized INI comments, not looked up in settings_locale.h.
        source = re.sub(r'const PhoneHelp PHONE_HELP\[\] = \{.*?\n\};', '', source, flags=re.S)
        # INI keys looked up in the settings table (ini("Mode"), lo("FPS")...) are not shown
        source = re.sub(r'\b(?:ini|cold|lo|hi|getenv)\(' + LITERAL + r'\)', '', source)
        unchanged = {"English", "Español", "中文", "Xbox", "Switch", "PlayStation",
                     "FSR", "MSAA", "FXAA", "Discord", "Settings##main", "Settings##dropdown", "Settings##bind",
                     "A", "B", "X", "Y", "L", "R", "ZL", "ZR",  # (the Switch buttons' keybinds)
                     "L3", "R3", "Guide", "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad",
                     "FSR 3.1", "FSR 4",  # (product names)
                     "ShowAttacks", "ShowDodge", "ShowGuard", "ShowSheathe", "ShowPlus", "ShowMinus", "ShowDpad",
                     "ShowL", "ShowZL", "ShowZR", "ShowL3", "ShowR3", "ShowStick", "ShowRanged", "ShowFire",
                     "ShowSecond", "ShowAim", "ShowRangedSpecial", "ShowAmmo", "ShowItemArrows", "ShowMenuZL",
                     "ShowMenuZR", "Combo", "Name"}  # (row/INI keys, including generated ComboNName, not labels)
        visible = {text for text in literals(source) if text and
                   (text[0].isupper() or " " in text) and any(c.isalpha() for c in text)}
        self.assertFalse(visible - set(keys) - unchanged,
                         f"Untranslated UI labels: {sorted(visible - set(keys) - unchanged)}")

        # the Game actions screen's labels (runtime/actions.c: an action's and a merged row's)
        actions = (ROOT / "runtime/actions.c").read_text(encoding="utf-8")
        labels = [ast.literal_eval(m.group(1)) for m in re.finditer(
            r'\{"\w+",\s*ACT_VIEW_\w+,[^"]*?(' + LITERAL + r')', actions)]
        labels += [ast.literal_eval(m.group(1)) for m in re.finditer(
            r'\{"\w+",\s*(' + LITERAL + r'),\s*\d,\s*ACT_', actions)]
        self.assertGreater(len(labels), 80)
        self.assertFalse(set(labels) - set(keys), f"Untranslated action labels: {sorted(set(labels) - set(keys))}")
        for group in ("Hunt and town", "Menus"):
            self.assertIn(group, keys)

        # notices on screen (runtime/net/ldn.c: UI_TEXT("..."))
        notices = literals(" ".join(re.findall(r'UI_TEXT\((' + LITERAL + r')\)',
                                               (ROOT / "runtime/net/ldn.c").read_text(encoding="utf-8"))))
        self.assertTrue(notices)
        self.assertFalse(set(notices) - set(keys), f"Untranslated notices: {sorted(set(notices) - set(keys))}")

        errors = (ROOT / "runtime/config_store.c").read_text(encoding="utf-8")
        messages = [literals(value)[-1] for value in re.findall(
            r'fail\([^;]*?' + LITERAL + r'\)', errors)]
        # the settings table's messages (runtime/config.c g_settings)
        table = (ROOT / "runtime/config.c").read_text(encoding="utf-8")
        messages += [literals(value)[0] for value in re.findall(r'#define ERR_\w+ +(' + LITERAL + ')', table)]
        self.assertTrue(messages)
        self.assertFalse(set(messages) - set(keys))

    def test_chinese_catalogs(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        # Product and standard names keep their spelling in the Chinese menu.
        technical_names = {"Ryujinx", "BT.1886", "HDR", "Android"}
        for english, _, translated in rows(catalog, 3):
            self.assertTrue(has_chinese(translated) or translated in technical_names, english)
        labels = chinese.split("static inline const char *settings_help_zh(", 1)[0]
        for english, translated in rows(labels, 2):
            self.assertTrue(has_chinese(translated) or translated in technical_names, english)
        self.assertNotIn(chr(0xfffd), chinese)

    def test_general_layout_display_names(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        overrides = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        for translations in (labels, overrides):
            self.assertEqual(translations["Melee layout 2"], "近战布局")
            self.assertEqual(translations["Ranged layout"], "远程布局")
        # Keep the legacy lookup key while using the same Chinese name in labels and help.
        self.assertNotRegex(catalog + chinese, r"近战布局\s*2")

    def test_common_template_and_combo_name_labels(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        labels.update(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        for key in ("Custom touch templates", "Template name", "Button name", "Template %d/40: %s",
                    "Save current layout to this slot", "Apply selected template",
                    "Saved template %d/40: %s", "Applied template %d/40: %s",
                    "The universal layout has six combined buttons. A template saves each button's combination, controller binding and name."):
            self.assertIn(key, labels)
            self.assertTrue(has_chinese(labels[key]), key)
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        self.assertIn('"touch.templates"', source)
        self.assertIn('"ce.name"', source)
        self.assertNotIn('"ce.weapon"', source)
        self.assertNotIn('"tp.bind"', source)
        self.assertNotIn('"touch.presets"', source)

    def test_touch_icons_and_template_detail_labels(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        labels.update(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        for key in ("Touch button icons", "Template details", "Default icon", "Custom image",
                    "Choose image from device", "Combined button %d", "Sword", "Crosshair",
                    "Icon saved to the current layout.",
                    "Could not import this image. The previous icon was kept."):
            self.assertIn(key, labels)
            self.assertTrue(has_chinese(labels[key]), key)
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        self.assertIn('push(View::TemplateDetail, "tp.save")', source)
        self.assertIn('"touch.icons"', source)
        self.assertIn('touch_layout_generation() == touch_icons.generation', source)

    def test_common_r_is_labeled_aim_with_legacy_visibility_key(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        overrides = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        for translations in (labels, overrides):
            self.assertEqual(translations["Aim (R)"], "瞄准 (R)")
            self.assertEqual(translations["Aim  R"], "瞄准  R")
            self.assertIn("Guard (R)", translations)  # Legacy lookup remains compatible.
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        titles = literals(source.split("TB_TITLES[TB_COUNT] = {", 1)[1].split("};", 1)[0])
        keys = literals(source.split("TB_KEYS[TB_COUNT] = {", 1)[1].split("};", 1)[0])
        self.assertEqual(titles[2], "Aim (R)")
        self.assertEqual(keys[2], "ShowGuard")
        phone_table = source.split("const PhoneHelp PHONE_HELP[] = {", 1)[1].split("\n};", 1)[0]
        aim_help = next(row for row in rows(phone_table, 4) if row[0] == "ShowGuard")
        self.assertIn("aim (R)", aim_help[1])
        self.assertIn("apuntar (R)", aim_help[2])
        self.assertIn("瞄准（R）", aim_help[3])
        self.assertIn("all native R functions", aim_help[1])
        self.assertIn("at the same time as automatic running", aim_help[1])
        self.assertIn("todas las funciones originales de R", aim_help[2])
        self.assertIn("a la vez", aim_help[2])
        self.assertIn("原生 R 的全部功能", aim_help[3])
        self.assertIn("与自动跑步同时使用", aim_help[3])
        self.assertNotIn("防御", aim_help[3])

    def test_auto_r_switch_and_help(self):
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        labels.update(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        self.assertEqual(labels["Full stick automatically runs"], "拉满摇杆自动跑步")
        auto_r = source.split('add_bool(r, "touch.auto_r",', 1)[1].split(";", 1)[0]
        self.assertIn('&c.touch_auto_r, ini("AutoR")', auto_r)
        help_key = literals(source.split('add_bool(r, "touch.auto_r",', 1)[1].split('r.back().help = tr(', 1)[1])[0]
        for phrase in ("fixed or floating", "when the game allows running", "with the weapon drawn it does not trigger",
                       "Manual R, combos and a controller's R work as before", "Default: Off"):
            self.assertIn(phrase, help_key)
        for phrase in ("固定或浮动", "游戏允许跑步", "1.60 倍半径", "拔刀时不触发", "默认：关闭"):
            self.assertIn(phrase, labels[help_key])
        floating_help = literals(source.split('add_bool(r, "touch.floating",', 1)[1].split('r.back().help = tr(', 1)[1])[0]
        self.assertIn("never the camera", floating_help)
        self.assertIn("Automatic running", floating_help)
        self.assertIn("separate switch", floating_help)
        self.assertNotIn("Full stick automatically holds R", source)
        self.assertNotIn("weapon drawn or sheathed", help_key)
        self.assertLess(source.index('"touch.floating"'), source.index('"touch.auto_r"'))
        help_rows = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[1], 2))
        self.assertIn("收刀时", help_rows["AutoR"])
        self.assertIn("1.60 倍半径", help_rows["AutoR"])
        self.assertIn("拔刀时不触发", help_rows["AutoR"])
        self.assertIn("默认：关闭", help_rows["AutoR"])

    def test_common_ammo_and_item_arrow_help(self):
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        overrides = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        for translations in (labels, overrides):
            self.assertEqual(translations["Next ammo"], "下一种弹药")
            self.assertEqual(translations["Previous ammo"], "上一种弹药")
        help_key = literals(source.split('action(r, "touch.buttons",', 1)[1].split('r.back().help = tr(', 1)[1])[0]
        self.assertIn("Item and ammo arrows never turn the camera", help_key)
        self.assertIn("only during quests with a bow or bowgun", help_key)
        for translations in (labels, overrides):
            for phrase in ("道具箭头和弹药箭头", "滑动不转视角", "仅任务内弓／弩可用", "六个组合键"):
                self.assertIn(phrase, translations[help_key])
        # Only the six original combo slots receive numbered titles; appended ammo icons use their own labels.
        title = source.split("std::string touch_icon_button_title(int button) {", 1)[1].split("\n}", 1)[0]
        self.assertIn("button >= 16 && button < 16 + TOUCH_COMBOS", title)
        self.assertIn("tr(touch_icon_button_name(button))", title)
        button_rows = source.split("void touch_button_rows(std::vector<Row> &r) {", 1)[1].split("\n}", 1)[0]
        self.assertIn("i >= TB_RANGED && i < TB_AMMO", button_rows)
        phone_table = source.split("const PhoneHelp PHONE_HELP[] = {", 1)[1].split("\n};", 1)[0]
        phone = {row[0]: row[1:] for row in rows(phone_table, 4)}
        ammo = phone["ShowAmmo"]
        for phrase in ("Independent", "universal layout", "only during quests with a bow or bowgun",
                       "never turns the camera", "Move and resize"):
            self.assertIn(phrase, ammo[0])
        for phrase in ("independientes", "solo en misiones", "nunca gira la cámara"):
            self.assertIn(phrase, ammo[1])
        help_rows = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[1], 2))
        for text in (ammo[2], help_rows["ShowAmmo"]):
            for phrase in ("通用布局", "独立", "仅任务内使用弓或弩炮", "滑动不转视角", "移动、缩放"):
                self.assertIn(phrase, text)
        self.assertIn("默认：开启", help_rows["ShowAmmo"])
        self.assertIn("never turns the camera", phone["ShowItemArrows"][0])
        self.assertIn("nunca gira la cámara", phone["ShowItemArrows"][1])
        for text in (phone["ShowItemArrows"][2], help_rows["ShowItemArrows"]):
            self.assertIn("滑动不转视角", text)
            self.assertIn("道具栏上滑动仍可切换道具", text)

    def test_removed_motion_settings_and_labels(self):
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        config = (ROOT / "runtime/config.c").read_text(encoding="utf-8")
        header = (ROOT / "runtime/config.h").read_text(encoding="utf-8")
        store = (ROOT / "runtime/config_store.c").read_text(encoding="utf-8")
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        default_keys, _ = default_ini_keys(config)
        help_rows = dict(rows(chinese.split("static inline const char *settings_help_zh(", 1)[1], 2))
        for key in ("Motion", "MotionGain", "MotionInvertX", "MotionInvertY"):
            self.assertNotIn(key, default_keys)
            self.assertNotIn(key, help_rows)
        self.assertNotRegex(source + config + header + store, r"\b(?:motion_(?:mode|gain|flip_x|flip_y)|config_motion_\w+|MOTION_(?:OFF|AIM|ALWAYS))\b")
        for label in ("Motion controls", "While aiming", "Motion sensitivity", "Invert motion horizontal", "Invert motion vertical"):
            self.assertNotIn(label, {row[0] for row in rows(catalog, 3)})
        self.assertNotIn("陀螺仪", chinese)
        self.assertIn("ObjectMotion", default_keys)
        self.assertIn("ObjectMotion", help_rows)

    def test_chinese_help_coverage(self):
        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        config = (ROOT / "runtime/config.c").read_text(encoding="utf-8")
        chinese = (ROOT / "runtime/settings_locale_zh.h").read_text(encoding="utf-8")
        phone_table = source.split("const PhoneHelp PHONE_HELP[] = {", 1)[1].split("\n};", 1)[0]
        phone = rows(phone_table, 4)
        self.assertGreaterEqual(len(phone), 47)
        self.assertEqual(len(phone), len(re.findall(r'\{', phone_table)))
        phone_keys = [key for key, _, _, _ in phone]
        self.assertEqual(len(phone_keys), len(set(phone_keys)), "Duplicate phone help key")
        for key, english, spanish, translated in phone:
            self.assertTrue(english.strip() and spanish.strip() and has_chinese(translated), key)
        self.assertIn("s.draft.language == 2 ? h.zh", source)
        self.assertIn("settings_help_zh(r.key.c_str())", source)

        default_keys, help_keys = default_ini_keys(config)
        for number in range(1, 7):
            self.assertIn(f"UniversalCombo{number}", default_keys)
            self.assertIn(f"UniversalCombo{number}Pad", default_keys)
            self.assertIn(f"Combo{number}Name", default_keys)
        table = config.split("const ConfigSetting g_settings[] = {", 1)[1].split("#undef ON_OFF", 1)[0]
        setting_keys = set(default_keys)
        for _, names in re.findall(r'\{\s*(' + LITERAL + r')\s*,\s*(' + LITERAL + r')', table):
            setting_keys.update(ast.literal_eval(names).split("|"))
        for names in re.findall(r'ON_OFF\(\s*' + LITERAL + r'\s*,\s*(' + LITERAL + r')', table):
            setting_keys.add(ast.literal_eval(names))
        self.assertFalse(set(phone_keys) - setting_keys, "Phone help names must be real settings")

        help_table = chinese.split("static inline const char *settings_help_zh(", 1)[1]
        help_rows = rows(help_table, 2)
        translations = {}
        for names, translated in help_rows:
            self.assertTrue(has_chinese(translated), names)
            for key in names.split("|"):
                self.assertNotIn(key, translations, "Duplicate Chinese help key")
                translations[key] = translated
        self.assertFalse(set(translations) - setting_keys, "Chinese help names must be real settings")
        # A row may supply a translated help sentence instead of its INI comments.
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        labels = {en: zh for en, _, zh in rows(catalog, 3)}
        labels.update(rows(chinese.split("static inline const char *settings_help_zh(", 1)[0], 2))
        explicit = set()
        pattern = (r'\b(?:ini|cold)\(\s*(' + LITERAL + r')\s*\)[^;]*;\s*'
                   r'(?:r\.back\(\)\.(?!help)[^;]*;\s*)*'
                   r'r\.back\(\)\.help\s*=\s*(?:tr|fmt)\(\s*(' + LITERAL + r')')
        for key, english in re.findall(pattern, source):
            if has_chinese(labels.get(ast.literal_eval(english), "")):
                explicit.add(ast.literal_eval(key))
        visible = {ast.literal_eval(key) for key in re.findall(
            r'\b(?:ini|cold)\(\s*(' + LITERAL + r')\s*\)', source)}
        missing = set()
        for key in help_keys | visible:
            # settings_help_zh strips weapon/layout prefixes before ComboN lookup.
            lookup = key[key.index("Combo"):] if "Combo" in key else key
            if lookup not in translations and key not in phone_keys and key not in explicit:
                missing.add(key)
        self.assertFalse(missing, f"Missing Chinese setting help: {sorted(missing)}")
        self.assertGreater(len(help_keys), 300)
        self.assertIn("通用", translations["Combo1"])
        self.assertRegex(translations["Combo1"], r"(?:6 个|六个)(?:独立)?组合键")
        self.assertIn("名称", translations["Combo1Name"])
        self.assertIn("模板", translations["Combo6Name"])


if __name__ == "__main__":
    unittest.main()
