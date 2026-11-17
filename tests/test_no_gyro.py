"""Keep retained gyro files inactive while preserving the stationary guest HID ABI."""
from pathlib import Path
import re


project = Path(__file__).resolve().parents[1]
roots = (project / "runtime", project / "android/app/src/main/java/com/mhgu")
retained = {project / "runtime/gyro.h", roots[1] / "relink/MotionSensor.java"}
removed = re.compile(
    r"\b(?:config_motion_\w+|mousecam_motion_\w+|host_input_motion_\w+|"
    r"motion_mode|motion_gain|motion_flip_[xy]|g_mot_yaw|g_mot_pitch|"
    r"MOTION_OFF|MOTION_AIM|MOTION_ALWAYS|MotionGain|MotionInvert[XY]|"
    r"gyro_sample|GyroTurn|MotionSensor|TYPE_GYROSCOPE|"
    r"SDL_SENSOR_GYRO(?:_[LR])?|SDL_EVENT_(?:GAMEPAD_)?SENSOR_UPDATE|SDL_INIT_SENSOR|"
    r"SDL_(?:GamepadHasSensor|GamepadSensorEnabled|SetGamepadSensorEnabled|"
    r"OpenSensor|GetSensors|GetSensorData)|"
    r"android\.hardware\.Sensor(?:Event|EventListener|Manager)?)\b"
)
violations = []
for root in roots:
    for path in sorted(root.rglob("*")):
        if path in retained or path.suffix not in {".c", ".cpp", ".h", ".java"}:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if removed.search(line) or re.search(r'^\s*#\s*include\s*["<][^">]*\bgyro\.h[">]', line):
                violations.append(f"{path.relative_to(project)}:{number}: {line.strip()}")
assert not violations, "Gyro input remains:\n" + "\n".join(violations)
# Historical files may remain, but active code must not reference or include them.

hid = (project / "runtime/hle/input.c").read_text(encoding="utf-8")
for handler in ("handles", "start", "stop", "rest", "state"):
    assert f"hid_sixaxis_{handler})" in hid, f"Guest six-axis ABI lost: {handler}"
assert re.search(r"hid_sixaxis_rest\)\s*\{\s*RET\(1\);", hid)
state = hid.split("hid_sixaxis_state) {", 1)[1].split("\n}", 1)[0]
assert "memset(s, 0, 0x60);" in state and "memcpy(s + 0x18 + 8, &one, 4);" in state
print("PASS: active code has no gyro settings, sensor subscriptions or references to retained gyro files; stationary guest six-axis ABI remains.")
