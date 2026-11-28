typedef struct {uint32_t magic,seq,english,quick,key,crc;} SettingsDisk;
static const char*settings_paths[]={"savedata:/gu_wheel_settings_a.bin","savedata:/gu_wheel_settings_b.bin"};
/* Bits 0..3 store the preset shortcut; bits 4..8 store one plus the
   hold button. Legacy records have zero upper bits and use the default L. */
static bool valid_binding_word(unsigned word){
 unsigned encoded=word>>4;
 return encoded<=16&&encoded!=6&&encoded!=11;
}
/* Older records may set bit 5. Accept it on read and omit it on save. */
static bool read_settings(const char*path,SettingsDisk*d){
 return read_exact(path,d,sizeof(*d))&&d->magic==0x31535747&&d->english<=1&&
        d->quick<64&&(d->quick&31)<24&&valid_binding_word(d->key);
}
/* Old records allowed both roles on one button. Preserve the wheel opener.
   When rebinding, prefer the previous opener for the other half of the chord. */
static unsigned switch_key_for_opener(unsigned key,unsigned opener,unsigned previous){
 if(key!=opener && key!=4 && key!=5)return key;
 if(previous!=opener && previous!=4 && previous!=5)return previous;
 return opener==9?6:9;
}
static void load_settings(void){
 SettingsDisk a,b;bool va=read_settings(settings_paths[0],&a),vb=read_settings(settings_paths[1],&b);
 SettingsDisk*d=vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:0;
 if(d){settings_sequence=d->seq;english=d->english;quick_enabled=d->quick&1;opacity=(d->quick>>1)&3;icon_style=(d->quick>>3)&3;quick_key=d->key&15;
  open_key=(d->key>>4)?(d->key>>4)-1:6;
  quick_key=switch_key_for_opener(quick_key,open_key,6);}
}
static bool save_bindings(unsigned en,unsigned quick,unsigned key,unsigned opener){
 SettingsDisk d={0x31535747,settings_sequence+1,en,quick|(opacity<<1)|(icon_style<<3),key|((opener+1)<<4),0},check;d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char*path=settings_paths[d.seq&1];bool ok=write_exact(path,&d,sizeof(d))&&read_settings(path,&check)&&check.seq==d.seq&&check.crc==d.crc;
 storage_end();if(ok){settings_sequence=d.seq;english=en;quick_enabled=quick;quick_key=key;open_key=opener;}return ok;
}
static bool save_settings(unsigned en,unsigned quick,unsigned key){
 return save_bindings(en,quick,key,open_key);
}
static void quick_next(void){
 for(unsigned n=1;n<=4;n++){
  unsigned i=(presets.active+n)%4;
  if(!(presets.valid&(1u<<i)))continue;
  if(activate_preset(i))((void(*)(unsigned,unsigned))(base+0x54f260))(0,4);
  return;
 }
}
