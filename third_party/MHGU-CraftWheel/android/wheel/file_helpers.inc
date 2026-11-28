static void copy(void *d,const void*s,unsigned n){unsigned char *a=d;const unsigned char*b=s;while(n--)*a++=*b++;}
void *memcpy(void*d,const void*s,unsigned n){copy(d,s,n);return d;}
static uint32_t crc(const void*p,unsigned n){const unsigned char*b=p;uint32_t c=~0u;while(n--){c^=*b++;for(unsigned k=0;k<8;k++)c=(c>>1)^(0xedb88320u&-(c&1));}return c;}
static void* method(void*p,unsigned i){return (*(void***)p)[i];}
static void* open_native_file(const char*path,unsigned mode){
 uint32_t hash=crc("MtFile",6)&0x7fffffff;
 void*dti=((void*(*)(uint32_t))(base+0x7aef68))(hash);
 if(!dti)return 0;
 void*f=((void*(*)(void*))method(dti,2))(dti);
 if(!f)return 0;
 if(!((bool(*)(void*,const char*,int,bool))method(f,6))(f,path,mode,false)){
  ((void(*)(void*))method(f,1))(f);return 0;
 }
 return f;
}
static void close_file(void*f){((void(*)(void*))method(f,7))(f);((void(*)(void*))method(f,1))(f);}

static bool storage_begin(void){return forge_fs_mountSaveData();}
static void storage_end(void){forge_fs_unmountSaveData();}
static void*open_file(const char*p,unsigned m){return open_native_file(p,m);}
static unsigned file_size(void*f){return ((unsigned(*)(void*))method(f,15))(f);}
static unsigned file_read(void*f,void*d,unsigned n){return ((unsigned(*)(void*,void*,unsigned))method(f,8))(f,d,n);}
static unsigned file_write(void*f,const void*d,unsigned n){return ((unsigned(*)(void*,const void*,unsigned))method(f,12))(f,d,n);}
