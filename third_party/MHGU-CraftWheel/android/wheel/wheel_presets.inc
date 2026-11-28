typedef struct {uint32_t magic,seq,valid,targets[5][8],crc;} PresetDisk;
static unsigned trigger_sequence;
static const char*preset_paths[]={"savedata:/gu_wheel_presets_a.bin","savedata:/gu_wheel_presets_b.bin"};
static const char*trigger_paths[]={"savedata:/gu_wheel_trigger_a.bin","savedata:/gu_wheel_trigger_b.bin"};
static bool read_exact(const char*path,void*d,unsigned size){
 void*f=open_file(path,0x11);if(!f)return false;
 unsigned sz=file_size(f),n=0;
 if(sz==size)n=file_read(f,d,size);
 close_file(f);return n==size&&crc(d,size-4)==*(uint32_t*)((uint8_t*)d+size-4);
}
static bool write_exact(const char*path,void*d,unsigned size){
 void*f=open_file(path,0x13);if(!f)return false;
 unsigned n=file_write(f,d,size);close_file(f);return n==size;
}
static bool read_presets(const char*path,PresetDisk*d){
 if(!read_exact(path,d,sizeof(*d))||d->magic!=0x31525750||d->valid>31)return false;
 for(unsigned i=0;i<5;i++)for(unsigned j=0;j<8;j++)if(d->targets[i][j]>=2991)return false;
 return true;
}
static bool read_trigger(const char*path,PositionDisk*d){return read_exact(path,d,sizeof(*d))&&d->magic==0x31545747&&d->left<=2;}
static void load_extra(void){
 bool va,vb;
 PositionDisk x,y;va=read_trigger(trigger_paths[0],&x);vb=read_trigger(trigger_paths[1],&y);
 PositionDisk*t=vb&&(!va||(int32_t)(y.seq-x.seq)>0)?&y:va?&x:0;
 release_trigger=t?t->left:0;trigger_sequence=t?t->seq:0;
}
#include "wheel_loadouts.inc"
static bool save_trigger(unsigned value){
 PositionDisk d={0x31545747,trigger_sequence+1,value,0},check;d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char*path=trigger_paths[d.seq&1];bool ok=write_exact(path,&d,sizeof(d))&&read_trigger(path,&check)&&check.seq==d.seq&&check.left==value;
 storage_end();if(ok){release_trigger=value;trigger_sequence=d.seq;}return ok;
}
