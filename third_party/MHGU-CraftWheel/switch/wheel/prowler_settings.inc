typedef struct {
 uint32_t magic,seq,enabled,trigger,opacity,x,y,scale,crc;
} CatOptionsDisk;
static const char *cat_paths[]={"savedata:/gu_wheel_cat_a.bin","savedata:/gu_wheel_cat_b.bin"};
static unsigned cat_sequence;

static bool read_cat_options(const char *path,CatOptionsDisk *d){
 if(!read_exact(path,d,sizeof(*d)) || d->magic!=0x31544357 ||
    d->enabled>1 || d->trigger>1 || d->opacity>3 || d->scale<50 || d->scale>150)return false;
 unsigned margin_x=(POSITION_MIN_X*d->scale+99)/100;
 unsigned margin_y=(POSITION_MIN_Y*d->scale+99)/100;
 return d->x>=margin_x && d->x<=1280-margin_x && d->y>=margin_y && d->y<=720-margin_y;
}
static void apply_cat_options(const CatOptionsDisk *d){
 cat_enabled=d->enabled; cat_trigger=d->trigger; cat_opacity=d->opacity;
 cat_x=d->x; cat_y=d->y; cat_scale=d->scale; cat_sequence=d->seq;
}
static void load_cat_options(void){
 CatOptionsDisk a,b;
 bool va=read_cat_options(cat_paths[0],&a),vb=read_cat_options(cat_paths[1],&b);
 static const CatOptionsDisk defaults={0,0,1,0,3,POSITION_DEFAULT_X,POSITION_DEFAULT_Y,100,0};
 apply_cat_options(vb&&(!va||(int32_t)(b.seq-a.seq)>0)?&b:va?&a:&defaults);
}
static bool save_cat_options(unsigned enabled,unsigned trigger,unsigned opacity_value,
                             unsigned x,unsigned y,unsigned scale_percent){
 CatOptionsDisk d={0x31544357,cat_sequence+1,enabled,trigger,opacity_value,x,y,scale_percent,0},check;
 d.crc=crc(&d,sizeof(d)-4);
 if(!storage_begin())return false;
 const char *path=cat_paths[d.seq&1];
 bool saved=write_exact(path,&d,sizeof(d))&&read_cat_options(path,&check)&&check.seq==d.seq&&check.crc==d.crc;
 storage_end();
 if(saved)apply_cat_options(&check);
 return saved;
}
static void change_cat_option(unsigned row){
 if(row==2){begin_position_edit();return;}
 bool ok=save_cat_options(row==0?!cat_enabled:cat_enabled,
                          row==1?!cat_trigger:cat_trigger,
                          row==3?(cat_opacity+1)%4:cat_opacity,
                          cat_x,cat_y,cat_scale);
 status=ok?"已保存":"设置保存失败";
}
