"""Exercise the shipped binding UI and settings code in ARM with simulated storage."""
from pathlib import Path
from tempfile import TemporaryDirectory
import argparse
import os
import struct
import subprocess

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_R0


parser = argparse.ArgumentParser()
parser.add_argument("project", type=Path)
parser.add_argument("--cc", default=os.environ.get("CC", "arm-none-eabi-gcc"))
args = parser.parse_args()
wheel = args.project / "wheel"
settings = (wheel / "wheel_settings.inc").read_text(encoding="utf-8")
settings = settings[:settings.index("static void quick_next(void){")]
plugin = (wheel / "wheel_plugin.c").read_text(encoding="utf-8")
begin = plugin.index(" if(wheel.editing&&mapping){") + len(" if(wheel.editing&&mapping){")
end = plugin.index("\n }", begin)
capture = plugin[begin:end]

harness = """
#include <stdint.h>
#include <stdbool.h>
static unsigned settings_sequence,english,quick_enabled,opacity,icon_style,quick_key,open_key=6;

static void load_touch_mode(void){} /* The separate Android mode journal is tested by test_callback.py. */
static unsigned release_mask,write_failure,mount_failure;
static bool mapping,mapping_wait,mapping_open_key,pointer_owned;
static const char*mapping_error,*status;
static uint32_t files[2][6];
static uint32_t crc(const void*src,unsigned n){
 const unsigned char*p=src;uint32_t result=~0u;
 while(n--){result^=*p++;for(int i=0;i<8;i++)result=(result>>1)^(0xedb88320u&-(result&1));}
 return ~result;
}
static uint32_t*slot(const char*p){
 while(*p&&*p!='.')p++;return files[p[-1]=='b'];
}
static bool read_exact(const char*p,void*d,unsigned n){
 uint32_t *record=slot(p);if(n!=24||crc(record,20)!=record[5])return false;
 for(unsigned i=0;i<6;i++)((uint32_t*)d)[i]=record[i];
 return true;
}
static bool write_exact(const char*p,void*d,unsigned n){
 uint32_t *record=slot(p);if(n!=24)return false;
 for(unsigned i=0;i<(write_failure?3:6);i++)record[i]=((uint32_t*)d)[i];
 return !write_failure;
}
static bool storage_begin(void){return !mount_failure;}
static void storage_end(void){}
""" + settings + """
static void capture(unsigned held,unsigned pressed){
""" + capture + """
}
#define CHECK(x) do{if(!(x))return __LINE__;}while(0)
int run(void){
 uint32_t *old=files[0];old[0]=0x31535747;old[1]=8;old[2]=1;
 old[3]=32|1|(2<<1)|(2<<3);old[4]=4;old[5]=crc(old,20);
 load_settings();

 CHECK(open_key==6&&quick_key==9&&settings_sequence==8&&english==1&&quick_enabled==1&&opacity==2&&icon_style==2);
 mapping=mapping_open_key=mapping_wait=true;capture(1,1);
 CHECK(mapping_wait&&open_key==6);
 capture(0,0);CHECK(!mapping_wait);capture(3,3);
 CHECK(mapping&&mapping_wait&&mapping_error&&open_key==6&&settings_sequence==8);
 capture(0,0);capture(32,32);CHECK(mapping&&mapping_wait&&open_key==6);
 capture(0,0);capture(1024,1024);CHECK(!mapping&&open_key==6&&settings_sequence==8&&release_mask==1024);
 for(unsigned k=0;k<16;k++){
  if(k==5||k==10)continue;
  unsigned mask=1u<<k;release_mask=0;mapping=mapping_open_key=true;mapping_wait=false;
  unsigned other=switch_key_for_opener(quick_key,k,open_key);
  capture(mask,mask);
  CHECK(!mapping&&!mapping_error&&release_mask==mask&&open_key==k&&quick_key==other);
  open_key=99;load_settings();
  CHECK(open_key==k&&quick_enabled==1&&english==1&&quick_key==other&&opacity==2&&icon_style==2);
 }
 CHECK(save_settings(0,0,13));open_key=99;quick_key=99;load_settings();
 CHECK(open_key==15&&quick_key==13&&english==0&&!quick_enabled&&opacity==2&&icon_style==2);
 unsigned seq=settings_sequence;
 mapping=mapping_open_key=true;mapping_wait=false;mount_failure=1;capture(1,1);
 CHECK(mapping&&mapping_wait&&mapping_error&&settings_sequence==seq&&open_key==15);
 mount_failure=0;write_failure=1;capture(0,0);capture(1,1);
 CHECK(mapping&&mapping_wait&&settings_sequence==seq&&open_key==15);
 open_key=99;load_settings();CHECK(open_key==15&&settings_sequence==seq);
 write_failure=0;capture(0,0);capture(2,2);
 CHECK(!mapping&&open_key==1&&settings_sequence==seq+1);
 // A CRC-valid but unsupported binding must also fall back to the older slot.
 uint32_t*corrupt=files[settings_sequence&1];corrupt[4]=0x6d;corrupt[5]=crc(corrupt,20);
 open_key=99;load_settings();CHECK(open_key==15&&settings_sequence==seq);
 mapping=true;mapping_open_key=false;mapping_wait=false;release_mask=0;capture(16,16);
 CHECK(!mapping&&release_mask==16&&quick_key==13&&open_key==15);
 CHECK(save_bindings(1,1,12,6));
 CHECK((files[settings_sequence&1][3]&32)==0);
 load_settings();
 CHECK(icon_style==2&&opacity==2&&english==1&&quick_enabled==1&&quick_key==12&&open_key==6);
 // A is now the opener and B is the preset key. Binding both roles to A is rejected.
 CHECK(save_bindings(1,1,1,0));
 mapping=true;mapping_open_key=false;mapping_wait=false;capture(1,1);
 CHECK(mapping&&mapping_wait&&mapping_error&&open_key==0&&quick_key==1);
 capture(0,0);capture(64,64);
 CHECK(!mapping&&quick_key==6&&open_key==0);
 mapping=mapping_open_key=true;mapping_wait=false;capture(64,64);
 CHECK(!mapping&&open_key==6&&quick_key==0);
 CHECK(save_bindings(1,1,0,0));load_settings();
 CHECK(open_key==0&&quick_key==6);
 return 0;
}
"""

with TemporaryDirectory() as temporary:
    temp = Path(temporary)
    cfile, elf = temp / "test.c", temp / "test.elf"
    cfile.write_text(harness, encoding="utf-8")
    subprocess.run([args.cc, "-marm", "-march=armv7-a", "-O2", "-nostdlib", "-ffreestanding",
                    "-fno-builtin", "-Wl,-Ttext=0x10000,-e,run", str(cfile), "-lgcc", "-o", str(elf)], check=True)
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.mem_map(0x8000, 0x40000)
    with elf.open("rb") as stream:
        image = ELFFile(stream)
        for segment in image.iter_segments():
            if segment["p_type"]=="PT_LOAD":
                cpu.mem_write(segment["p_vaddr"], segment.data())
        entry = image.header.e_entry
    stop = 0x47ff0
    cpu.reg_write(UC_ARM_REG_SP, 0x47000)
    cpu.reg_write(UC_ARM_REG_LR, stop)
    cpu.emu_start(entry, stop, count=1000000)
    assert cpu.reg_read(UC_ARM_REG_PC)==stop
    line = cpu.reg_read(UC_ARM_REG_R0)
    assert line==0, f"Harness failed at line {line}: {harness.splitlines()[line-1]}"
print("PASS: migrated legacy settings, captured all 14 keys, rejected chords/reserved input, cancelled, preserved other options, recovered from failed writes and a corrupt slot")
