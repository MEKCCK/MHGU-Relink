/* One touch owns the wheel from a new press inside the pack until lift.
   Coordinates are the same fitted 1280x720 units used by the wheel preview. */
static bool bag_available, bag_was_down, bag_captured, bag_outer, bag_quest;
static unsigned bag_pressed_at;
static float bag_origin_x, bag_origin_y;
static float bag_display_w, bag_display_h;

static void bag_suspend(void){
    if(bag_captured && !wheel.editing)
        wheel_step(bag_quest,0,0,0,0,0,0);
    bag_available=false;
    bag_captured=false;
    bag_outer=false;
    /* A finger already held over the menu must lift before a new capture. */
    bag_was_down=true;
}

static bool bag_display_changed(float w,float h){
    bool changed=w!=bag_display_w || h!=bag_display_h;
    bag_display_w=w;
    bag_display_h=h;
    return changed;
}

static void bag_step(bool in_quest, unsigned now, const Slot *items){
    unsigned center_x=playing_prowler?cat_x:position_x;
    unsigned center_y=playing_prowler?cat_y:position_y;
    unsigned percent=playing_prowler?cat_scale:position_percent;
    if(bridge_pointer_cancelled){
        bag_suspend();
        return;
    }
    float x=0,y=0;
    bool down=wheel_touch_get(&x,&y);
    if(down && !(x>=-32768 && x<=32768 && y>=-32768 && y<=32768)){
        bag_suspend();
        return;
    }
    if(bag_available && bag_quest!=in_quest)bag_suspend();
    bag_quest=in_quest;
    bag_available=true;
    if(down && !bag_was_down){
        float dx=x-center_x,dy=y-center_y;
        bag_captured=dx*dx+dy*dy<=36*36;
        if(bag_captured){
            bag_origin_x=x;
            bag_origin_y=y;
            bag_pressed_at=now;
            bag_outer=false;
        }
    }
    bag_was_down=down;
    if(!bag_captured){
        wheel_step(in_quest,0,0,0,0,now,items);
        return;
    }
    unsigned held_button=1u<<open_key;
    if(!down){
        /* Lifting has the same neutral edge as releasing a stick. Forward
           that action once, then close before the next render callback. */
        unsigned use=0,item=0;
        if(wheel.visible && wheel_trigger_now()==1){
            wheel_step(in_quest,held_button,0,0,0,now,items);
            use=wheel.action;
            item=wheel.action_item;
        }
        wheel_step(in_quest,0,0,0,0,now,items);
        wheel.action=use;
        wheel.action_item=item;
        bag_captured=false;
        bag_outer=false;

        return;
    }
    float dx=x-bag_origin_x,dy=bag_origin_y-y;
    float r2=dx*dx+dy*dy;
    if(!wheel.visible){
        /* Sliding off before the hold completes cancels a stray press. */
        if(now-bag_pressed_at<200 && r2>24*24){
            bag_captured=false;
            wheel_step(in_quest,0,0,0,0,now,items);
            return;
        }
        wheel_step(in_quest,in_quest?held_button:0,0,0,0,now,items);
        if(!wheel.visible)return;
    }
    /* Scale by the larger component beyond the nominal stick span. This
       preserves octant boundaries even when a finger slides off the ring. */
    dx*=100.0f/(float)percent;
    dy*=100.0f/(float)percent;
    r2=dx*dx+dy*dy;
    float ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
    float span=ax>ay?ax:ay;
    if(span<48)span=48;
    bool click=false;
    if(!bag_outer && r2>=68*68){
        click=wheel_trigger_now()==2;
        bag_outer=true;
    }else if(r2<52*52)bag_outer=false;
    wheel_step(in_quest,held_button,click?32u:0u,
               (int)(dx/span*32767),(int)(dy/span*32767),now,items);
}
