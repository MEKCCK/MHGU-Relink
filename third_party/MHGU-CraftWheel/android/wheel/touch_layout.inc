/* Design coordinates in the same aspect-fitted canvas as wheel_touch_get. */
extern const unsigned touch_detail_rows[7];
static const char *touch_trigger_names[]={"滑动触发","松手或回中触发","滑至外圈确认"};
typedef struct {float x,y;} TouchPoint;
static const TouchPoint slot_centers[8]={
 {640,235},{750,345},{640,455},{530,345},
 {717.782f,267.218f},{717.782f,422.782f},{562.218f,422.782f},{562.218f,267.218f}
};
static const TouchPoint touch_pad_centers[4]={{138,360},{198,300},{258,360},{198,420}};
static const TouchPoint confirm_center={1090,324},back_center={1010,426};
static bool hit_circle(float x,float y,TouchPoint center,float radius){
 x-=center.x;y-=center.y;return x*x+y*y<=radius*radius;
}
static bool hit_rect(float x,float y,float left,float top,float width,float height){
 return x>=left&&y>=top&&x<left+width&&y<top+height;
}
enum {
 TOUCH_NONE,TOUCH_SWITCH,TOUCH_CLOSE,TOUCH_DETAILS,TOUCH_OK,TOUCH_CANCEL,TOUCH_DEFAULT,TOUCH_CAT_SETTINGS,
 TOUCH_SAVE_PRESET,TOUCH_LOAD_PRESET,TOUCH_CLEAR,
 TOUCH_PRESET=16,TOUCH_SLOT=32,TOUCH_DETAIL=48,TOUCH_PAD=64,TOUCH_PICK=80,TOUCH_BIND=128
};

static TouchPoint bind_key_corner(unsigned key){
 return (TouchPoint){310+82*(key%8),326+62*(key/8)};
}
