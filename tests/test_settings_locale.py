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
                     "ShowMenuZR"}  # (row keys: settings_lookup / PHONE_HELP names, not labels)
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
        self.assertIn("0.1 到 5", translations["MotionGain"])
        self.assertIn("默认：1.0", translations["MotionGain"])
        self.assertIn("左右", translations["MotionInvertX"])
        self.assertIn("上下", translations["MotionInvertY"])


if __name__ == "__main__":
    unittest.main()
