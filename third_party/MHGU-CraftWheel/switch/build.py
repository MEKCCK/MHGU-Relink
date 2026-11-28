from pathlib import Path
import argparse,subprocess,runpy
p=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('--cc',default='arm-none-eabi-gcc');a=ap.parse_args()
w=p/'wheel';pre=p/'presets'
subprocess.run([a.cc,'-marm','-march=armv7-a','-mfloat-abi=softfp','-mfpu=vfpv3-d16','-fPIC','-ffreestanding','-fno-builtin','-O2','-nostdlib','-nostartfiles','-Wl,-z,text','-shared','-Wl,-Bsymbolic,--hash-style=sysv,--build-id=sha1','-Wl,-T,'+str(pre/'switch32.ld'),str(pre/'crt0.s'),str(w/'wheel_plugin.c'),str(w/'wheel_state.c'),'-lgcc','-o',str(w/'wheel.elf')],check=True)
runpy.run_path(str(p.parent/'elf_to_nro.py'),init_globals={'wheel_dir':w})
