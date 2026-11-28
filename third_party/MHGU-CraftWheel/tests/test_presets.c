#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t magic,version,seq,targets[8],crc; } Disk;
typedef struct { uint32_t magic,seq,left,crc; } PositionDisk;
static struct { uint32_t targets[8]; } wheel;
static uint32_t preview_targets[8],preset_row,ui_focus,release_trigger;
static bool previewing, fail_mount, fail_write;
static const char *status;
static void copy(void *dest,const void *src,unsigned size){
    for(unsigned i=0;i<size;i++)((uint8_t*)dest)[i]=((const uint8_t*)src)[i];
}
void *memcpy(void *d,const void *s,unsigned n){copy(d,s,n);return d;}
void *memset(void *p,int c,unsigned n){
    for(unsigned i=0;i<n;i++)((uint8_t*)p)[i]=c;
    return p;
}
static bool same_string(const char *a,const char *b){
    while(*a && *a==*b){a++;b++;}
    return *a==*b;
}
static uint32_t crc(const void *data,unsigned size){
    uint32_t sum=~0u;
    const uint8_t *p=data;
    while(size--){sum^=*p++;for(unsigned i=0;i<8;i++)sum=(sum>>1)^(0xedb88320u&-(sum&1u));}
    return ~sum;
}
static struct FakeFile {const char *path;uint32_t size,bytes[64];} files[8];
static void *open_file(const char *path,unsigned mode){
    for(unsigned i=0;i<8;i++)
        if(files[i].path && same_string(path,files[i].path))return &files[i];
    if(mode==0x13)for(unsigned i=0;i<8;i++)if(!files[i].path){files[i].path=path;return &files[i];}
    return 0;
}
static unsigned file_size(void *f){return ((struct FakeFile *)f)->size;}
static unsigned file_read(void *f,void *d,unsigned n){copy(d,((struct FakeFile *)f)->bytes,n);return n;}
static unsigned file_write(void *f,const void *d,unsigned n){
    struct FakeFile *out=f;
    if(fail_write)n/=2;
    if(n>sizeof(out->bytes))return 0;
    copy(out->bytes,d,n);
    return out->size=n;
}
static void close_file(void *f){(void)f;}
static bool storage_begin(void){return !fail_mount;}
static void storage_end(void){}
#include "wheel_presets.inc"

#define CHECK(e) do { if(!(e))return __LINE__; } while(0)
static void old_slots(unsigned valid){
    PresetDisk old={0};
    old.magic=0x31525750;old.seq=1;old.valid=valid;
    for(unsigned i=0;i<4;i++)if(valid&(1u<<i))old.targets[i][0]=100+i;
    old.crc=crc(&old,sizeof(old)-4);
    write_exact(preset_paths[1],&old,sizeof(old));
}
int run_test(void){
    Disk live={0};
    load_presets(0);
    CHECK(presets.active==0 && presets.valid==1 && wheel.targets[0]==0);
    old_slots(12);
    load_presets(0);
    CHECK(presets.active==2 && wheel.targets[0]==102);
    live.targets[0]=103;
    load_presets(&live);
    CHECK(presets.active==3 && presets.valid==12 && wheel.targets[0]==103);
    live.targets[0]=227;
    load_presets(&live);
    CHECK(presets.active==0 && presets.valid==13 && presets.targets[0][0]==227);
    old_slots(15);
    load_presets(&live);
    CHECK(presets.active==0 && presets.targets[0][0]==100 && wheel.targets[0]==227);
    select_preset_row(1);
    CHECK(previewing && preview_targets[0]==101 && wheel.targets[0]==227);
    preview_targets[7]=456;
    CHECK(save_preset(1,preview_targets) && wheel.targets[0]==227);
    load_presets(0);
    CHECK(presets.active==0 && presets.live[0]==227 && presets.targets[0][0]==100 && presets.targets[1][7]==456);
    fail_mount=true;
    CHECK(!activate_preset(1) && presets.active==0 && wheel.targets[0]==227);
    fail_mount=false;fail_write=true;
    CHECK(!activate_preset(1) && presets.active==0 && wheel.targets[0]==227);
    fail_write=false;
    load_presets(0);
    CHECK(presets.active==0 && wheel.targets[0]==227);
    CHECK(activate_preset(1) && presets.active==1 && preset_row==1 && !previewing && wheel.targets[7]==456);
    load_presets(0);
    CHECK(presets.active==1 && wheel.targets[0]==101 && wheel.targets[7]==456);
    uint32_t edit[8]={0,105};
    CHECK(save_preset(1,edit));
    load_presets(0);
    CHECK(wheel.targets[0]==0 && wheel.targets[1]==105 && presets.targets[1][1]==105);
    struct FakeFile *newest=open_file(loadout_paths[presets.seq&1],0x11);
    newest->bytes[5]^=0x41;  /* the previous intact copy must still load */
    load_presets(0);
    CHECK(presets.active==1 && wheel.targets[7]==456 && wheel.targets[0]==101);
    memset(files,0,sizeof(files));
    load_presets(0);
    CHECK(activate_preset(3) && presets.valid==9 && presets.active==3 && !previewing);
    load_presets(0);
    CHECK(presets.active==3 && wheel.targets[0]==0);
    CHECK(!activate_preset(4) && !save_preset(4,edit));
    return 0;
}
