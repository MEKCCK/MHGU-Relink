"""Windows-native equivalent of the supplied hardware Makefile."""
from pathlib import Path
import subprocess,concurrent.futures,sys,json
P=Path(__file__).resolve().parent;R=P/'hardware_forge';B=R/'build';B.mkdir(exist_ok=True)
TC=Path('D:/vibe coding/2026-10-19/z/work/builddeps/sdk/opt/devkitpro/devkitARM/bin')
cc=TC/'arm-none-eabi-gcc.exe';cxx=TC/'arm-none-eabi-g++.exe';ar=TC/'arm-none-eabi-ar.exe'
header=R/'data/version.h.in';t=header.read_text()
for k,v in {'FORGE_VERSION':'1.5.1-wheelcore-hardware','FORGE_VERSION_MAJOR':'1','FORGE_VERSION_MINOR':'5','FORGE_VERSION_PATCH':'1','FORGE_VERSION_COMMIT':'wheelcore','FORGE_VERSION_IS_RELEASE':'1'}.items():t=t.replace('@'+k+'@',v)
(B/'forge').mkdir(exist_ok=True);(B/'forge/version.h').write_text(t)
arch=['-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-mfloat-abi=softfp','-mfpu=crypto-neon-fp-armv8','-fPIC','-fno-short-enums']
common=arch+['-O2','-g','-ffunction-sections','-fdata-sections','-D__SWITCH__','-DLIBNX_NO_DEPRECATION','-DFORGE_INJECTED_NSO','-DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS','-DIMGUI_DISABLE_TTY_FUNCTIONS']
nx=R/'libs/libnx/nx'
includes=[R/'include',R/'libs/nnsdk/include',R/'libs/iniparser/src',nx/'external/bsd/include',R/'libs/nnheaders/include',R/'libs/nnheaders/src/NintendoSDK',R/'libs/cimgui/imgui',nx/'include',B]
incs=['-I'+str(f) for f in includes]
cpp=['-std=c++23','-fno-rtti','-fomit-frame-pointer','-fno-exceptions','-fno-asynchronous-unwind-tables','-fno-unwind-tables','-enable-libstdcxx-allocator=new','-fpermissive','-fno-threadsafe-statics','-include',str(R/'include/compat.h'),'-Wno-deprecated-declarations','-fno-builtin-ceil','-fno-builtin-ceilf','-fno-builtin-floor','-fno-builtin-floorf']
jobs=[];objects=[];nxobjects=[]
def add(f,kind):
 dest=B/(kind+'_'+str(f.relative_to(R)).replace('\\','_').replace('/','_')+'.o')
 flags=common+incs+(['-ftls-model=local-exec','-iquote',str(nx/'include/switch')] if kind=='nx' else ['-ftls-model=initial-exec'])
 compiler=cxx if f.suffix=='.cpp' else cc
 if f.suffix=='.cpp':flags+=cpp
 if f.suffix.lower()=='.s':flags+=['-x','assembler-with-cpp']
 jobs.append((f,dest,[str(compiler),*flags,'-c',str(f),'-o',str(dest)]))
 (nxobjects if kind=='nx' else objects).append(dest)
for directory in ['source/arm','source/kernel','source/sf','source/runtime']:
 for f in (nx/directory).iterdir():
  if f.suffix not in ['.s','.c','.cpp'] or f.name in ['cache.s','svc.s','exception.s','readtp.s','switch_crt0.s','switch32_crt0.s']:continue
  add(f,'nx')
for directory in ['source','source/forge','source/forge/graphics','libs/cimgui','libs/cimgui/imgui','libs/libstdcxx-pic','libs/iniparser/src']:
 for f in (R/directory).iterdir():
  if f.suffix in ['.s','.c','.cpp'] and f.name!='cimgui_impl.cpp':add(f,'forge')
def run(job):
 f,dest,args=job
 if dest.exists() and dest.stat().st_mtime>max(f.stat().st_mtime,header.stat().st_mtime):return
 r=subprocess.run(args,capture_output=True,text=True)
 if r.returncode:raise RuntimeError(str(f)+'\n'+r.stdout+'\n'+r.stderr)
 print('compiled',f.relative_to(R),flush=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as ex:
 for _ in ex.map(run,jobs):pass
lib=B/'libnx32_min.a';subprocess.run([str(ar),'rcs',str(lib),*map(str,nxobjects)],check=True)
math='sinf logf powf cosf fmodf acosf floorf sqrtf ceilf log pow sqrt atan2f'.split()
args=[str(cxx),*arch,'-nostartfiles','-nodefaultlibs','-shared','-Wl,--gc-sections,--build-id=sha1,-z,text','-Wl,-T,'+str(R/'switch32.ld'),'-Wl,--version-script='+str(R/'exported.txt'),'-Wl,-init=forge_init,-fini=forge_fini,--export-dynamic','-Wl,-Map,'+str(B/'subsdk.map'),*['-Wl,--wrap='+s for s in math],*map(str,objects),str(lib),'-lgcc','-u','malloc','-o',str(R/'subsdk.elf')]
subprocess.run(args,check=True)
subprocess.run([sys.executable,str(R/'tools/elf2nso32.py'),str(R/'subsdk.elf'),str(R/'subsdk.nso')],check=True)
subprocess.run([sys.executable,str(R/'tools/prepare_npdm.py')],check=True)
print('Hardware-fixed Forge 1.5.1 built and relocation checks passed',flush=True)
