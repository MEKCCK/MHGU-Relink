/* A separate journal lets older builds continue to share the ordinary wheel settings. */
typedef struct { uint32_t magic, sequence, enabled, crc; } TouchModeDisk;
static const char *touch_mode_paths[2]={"savedata:/gu_wheel_touch_mode_a.bin","savedata:/gu_wheel_touch_mode_b.bin"};
static unsigned touch_mode_sequence;
static bool read_touch_mode(const char *path,TouchModeDisk *out){
 return read_exact(path,out,sizeof(*out)) && out->magic==0x314d5447 && out->enabled<=1;
}
static void load_touch_mode(void){
 TouchModeDisk a,b;
 bool valid_a=read_touch_mode(touch_mode_paths[0],&a),valid_b=read_touch_mode(touch_mode_paths[1],&b);
 TouchModeDisk *latest=valid_b&&(!valid_a||(int32_t)(b.sequence-a.sequence)>0)?&b:valid_a?&a:0;
 touch_mode=latest&&latest->enabled;
 touch_mode_sequence=latest?latest->sequence:0;
}
static bool switch_touch_mode(void){
 TouchModeDisk next={0x314d5447,touch_mode_sequence+1,!touch_mode,0},check;
 next.crc=crc(&next,sizeof(next)-4);
 if(!storage_begin())return false;
 const char *path=touch_mode_paths[next.sequence&1];
 bool saved=write_exact(path,&next,sizeof(next))&&read_touch_mode(path,&check)&&check.crc==next.crc&&check.sequence==next.sequence;
 storage_end();
 if(saved){
  touch_mode=next.enabled;touch_mode_sequence=next.sequence;detail_index=0;
  forge_log(1,"[CraftWheel] saved input mode: %s",touch_mode?"touch":"controller");
 }
 return saved;
}
