static void copy(void *d,const void*s,unsigned n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;}
void *memcpy(void*d,const void*s,unsigned n){copy(d,s,n);return d;}
static uint32_t crc(const void*p,unsigned n){const unsigned char*b=p;uint32_t c=~0u;while(n--){c^=*b++;for(unsigned k=0;k<8;k++)c=(c>>1)^(0xedb88320u&-(c&1));}return c;}

/* SDK ARM32 FileHandle is four bytes. Avoid MtFile: its path rewriting
   routes custom mounts through the game's own filesystem policy. */
typedef struct {uint32_t value;} WheelFile;
extern unsigned wheel_mount_sd(const char*) __asm__("_ZN2nn2fs11MountSdCardEPKc");
extern unsigned sdk_open(WheelFile*,const char*,int) __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
extern unsigned sdk_create(const char*,int64_t) __asm__("_ZN2nn2fs10CreateFileEPKcx");
extern void sdk_close(WheelFile) __asm__("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
extern unsigned sdk_size(int64_t*,WheelFile) __asm__("_ZN2nn2fs11GetFileSizeEPxNS0_10FileHandleE");
extern unsigned sdk_resize(WheelFile,int64_t) __asm__("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEx");
extern unsigned sdk_read(unsigned*,WheelFile,int64_t,void*,unsigned) __asm__("_ZN2nn2fs8ReadFileEPjNS0_10FileHandleExPvj");
extern unsigned sdk_write(WheelFile,int64_t,const void*,unsigned,const unsigned*) __asm__("_ZN2nn2fs9WriteFileENS0_10FileHandleExPKvjRKNS0_11WriteOptionE");
static bool wheel_sd_ready;
static WheelFile wheel_file;
static bool storage_begin(void){
 if(!wheel_sd_ready){unsigned rc=wheel_mount_sd("gucw");wheel_sd_ready=rc==0;forge_log(3,"[CraftWheel] SD settings mount result=%08X",rc);}
 return wheel_sd_ready;
}
static void storage_end(void){}
static void*open_file(const char*path,unsigned mode){
 if(!wheel_sd_ready)return 0;
 char sdpath[96];const char*prefix="gucw:/";unsigned i=0;
 for(;prefix[i];i++)sdpath[i]=prefix[i];
 const char*tail=path;for(unsigned k=0;k<10&&*tail;k++)tail++;
 while(*tail&&i<sizeof(sdpath)-1)sdpath[i++]=*tail++;
 sdpath[i]=0;
 if(mode!=0x11){
  unsigned rc=sdk_create(sdpath,0);
  forge_log(3,"[CraftWheel] SD create result=%08X path=%s",rc,sdpath);
  /* ResultPathAlreadyExists (2002-0002) is expected on alternating writes. */
  if(rc&&rc!=0x402)return 0;
 }
 unsigned rc=sdk_open(&wheel_file,sdpath,mode==0x11?1:2);
 if(rc){forge_log(3,"[CraftWheel] SD open result=%08X path=%s",rc,sdpath);return 0;}
 return &wheel_file;
}
static void close_file(void*f){sdk_close(*(WheelFile*)f);}
static unsigned file_size(void*f){int64_t size=0;unsigned rc=sdk_size(&size,*(WheelFile*)f);return !rc&&size>=0&&size<=0xffffffffu?(unsigned)size:0;}
static unsigned file_read(void*f,void*d,unsigned size){unsigned n=0;unsigned rc=sdk_read(&n,*(WheelFile*)f,0,d,size);return rc?0:n;}
static unsigned file_write(void*f,const void*d,unsigned size){
 unsigned rc=sdk_resize(*(WheelFile*)f,size);
 if(!rc){const unsigned flush=1;rc=sdk_write(*(WheelFile*)f,0,d,size,&flush);}
 forge_log(3,"[CraftWheel] SD write result=%08X bytes=%u",rc,size);
 return rc?0:size;
}
static void* method(void*p,unsigned i){return (*(void***)p)[i];}
