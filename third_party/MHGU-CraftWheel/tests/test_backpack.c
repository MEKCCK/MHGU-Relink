#include <stdbool.h>
#include <stdint.h>
#include "wheel_state.c"

static bool pressed;
static bool bridge_pointer_cancelled;
static unsigned bridge_area;
static float bridge_pointer_x,bridge_pointer_y;
static float finger_x, finger_y;
static unsigned position_x=1113, position_y=360, position_percent=100;
static unsigned cat_x=850,cat_y=490,cat_scale=150;

static Slot pouch_slots[40];
static bool wheel_touch_get(float *x,float *y){
    *x=finger_x;
    *y=finger_y;
    return pressed;
}

#include "backpack_gesture.inc"

static void frame(unsigned time,bool down,int dx,int dy){
    pressed=down;
    finger_x=(float)position_x+dx;
    finger_y=(float)position_y+dy;
    bag_step(true,time,pouch_slots);
}

#define CHECK(cond) do { if(!(cond))return __LINE__; } while(0)
int test_backpack(void){
    const int dx[]={0,40,0,-40,40,40,-40,-40};
    const int dy[]={-40,0,40,0,-40,40,40,-40};
    wheel_reset();
    for(unsigned i=0;i<8;i++)wheel.targets[i]=i+51;
    for(unsigned trigger=0; trigger<3;trigger++){
        release_trigger=trigger;
        for(unsigned i=0;i<8;i++){
            unsigned t=1000*(i+8*trigger);
            frame(t,true,0,0); CHECK(!wheel.visible && bag_captured);
            frame(t+199,true,0,0); CHECK(!wheel.visible);
            frame(t+200,true,0,0); CHECK(wheel.visible && wheel_centered && !wheel.action);
            frame(t+201,true,dx[i],dy[i]); CHECK(wheel.action==1 && wheel.action_item==51+i && wheel.direction==i);
            frame(t+350,true,dx[i],dy[i]); CHECK(!wheel.action);
            frame(t+351,true,dx[i],dy[i]); CHECK(wheel.action==1 && wheel.action_item==51+i);
            if(trigger==2){
                frame(t+352,true,dx[i]*2,dy[i]*2); CHECK(wheel.action==12 && wheel.action_item==51+i);
                frame(t+353,true,dx[i]*2,dy[i]*2); CHECK(!wheel.action);
                frame(t+354,true,dx[i]/2,dy[i]/2); CHECK(!bag_outer);
                frame(t+355,true,dx[i]*2,dy[i]*2); CHECK(wheel.action==12 && wheel.action_item==51+i);
            }
            frame(t+360,false,0,0);
            CHECK(!wheel.visible && !bag_captured && !wheel.editing);
            CHECK(wheel.action==(trigger==1?9:0));
            if(trigger==1)CHECK(wheel.action_item==51+i);
            frame(t+361,false,0,0); CHECK(!wheel.action);
        }
    }
    release_trigger=1;
    frame(30000,true,0,0);frame(30200,true,0,0);frame(30210,true,40,0);
    frame(30220,true,0,0);CHECK(wheel.action==9&&wheel.action_item==52);
    frame(30230,false,0,0);CHECK(!wheel.action); /* No duplicate on lift. */
    frame(31000,true,0,0);frame(31200,true,0,0);frame(31210,true,-40,0);
    bag_suspend(); CHECK(!wheel.visible&&!wheel.action);
    frame(31220,true,0,0);frame(31400,false,0,0);CHECK(!wheel.visible&&!wheel.editing);
    frame(32000,true,60,0);frame(32001,true,0,0);frame(32400,true,0,0);
    frame(32401,false,0,0);CHECK(!wheel.visible&&!wheel.action&&!wheel.editing);
    frame(33000,true,0,0);frame(33100,true,35,0);frame(33400,true,35,0);
    frame(33410,false,0,0);CHECK(!wheel.visible&&!wheel.editing);
    frame(34000,true,0,0);frame(34190,false,0,0);CHECK(!wheel.editing);
    frame(34500,true,0,0);frame(34525,false,0,0);CHECK(!wheel.editing); /* Too late to pair. */
    frame(34600,true,0,0);frame(34625,false,0,0);CHECK(!wheel.editing);
    /* Pocket moves with the saved wheel center. Old coordinates do nothing. */
    position_x=256;position_y=567;
    frame(35000,true,857,-207);frame(35300,true,857,-207);
    frame(35310,false,0,0);CHECK(!wheel.visible && !wheel.editing);
    frame(35400,true,0,0);frame(35600,true,0,0);CHECK(wheel.visible);
    frame(35610,true,40,0);CHECK(wheel.direction==1);
    pressed=true;bag_step(false,35620,pouch_slots);CHECK(!wheel.visible && !wheel.action);
    pressed=false;bag_step(false,35630,pouch_slots);CHECK(!wheel.editing);
    pressed=true;bag_step(false,35640,pouch_slots); /* Finger was right of pack. */
    pressed=false;bag_step(false,35650,pouch_slots);CHECK(!wheel.editing);
    finger_x=position_x;finger_y=position_y;
    pressed=true;bag_step(false,35660,pouch_slots);
    pressed=false;bag_step(false,35730,pouch_slots);CHECK(!wheel.editing);
    pressed=true;bag_step(false,35755,pouch_slots);
    pressed=false;bag_step(false,35800,pouch_slots);CHECK(!wheel.editing && !wheel.visible);
    /* A second press can still be held to use the quest wheel. */
    frame(36000,false,0,0);
    frame(36020,true,0,0);frame(36045,false,0,0);
    frame(36110,true,0,0);frame(36310,true,0,0);CHECK(wheel.visible);
    frame(36330,false,0,0);CHECK(!wheel.editing && !wheel.visible);
    frame(36410,true,0,0);frame(36430,false,0,0);CHECK(!wheel.editing);
    bag_suspend();frame(36460,false,0,0);
    frame(36490,true,0,0);frame(36530,false,0,0);CHECK(!wheel.editing);
    frame(36560,true,0,0);frame(36575,true,35,0);frame(36600,false,35,0);
    frame(36620,true,0,0);frame(36655,false,0,0);CHECK(!wheel.editing);
    release_trigger=2;
    for(unsigned percent=50;percent<=150;percent+=100){
        unsigned t=40000+percent*10;
        position_percent=percent;
        frame(t,true,0,0);frame(t+200,true,0,0);CHECK(wheel.visible);
        frame(t+225,true,(int)(66*percent/100),0);CHECK(wheel.direction==1 && !bag_outer);
        frame(t+250,true,(int)(70*percent/100),0);CHECK(wheel.action==12 && bag_outer);
        frame(t+280,false,0,0);CHECK(!wheel.visible && !wheel.editing);
    }
    bridge_area=1;
    bridge_pointer_x=position_x;bridge_pointer_y=position_y;
    frame(43000,true,0,0);frame(43030,false,0,0);
    frame(43070,true,0,0);bridge_pointer_x+=55;frame(43100,false,55,0);
    CHECK(!wheel.editing);
    /* Prowler swipes use the independent saved center, size and trigger.
       The hunter origin and click trigger are still set above. */
    bridge_area=0;
    playing_prowler=wheel_cat_enabled=1;
    cat_actions[1]=25;
    frame(44000,true,0,0);frame(44240,true,0,0);
    CHECK(!bag_captured && !wheel.visible);
    frame(44300,false,0,0);
    int cx=(int)cat_x-(int)position_x, cy=(int)cat_y-(int)position_y;
    for(unsigned trigger=0;trigger<=1;trigger++){
        unsigned t=45000+trigger*1000;
        wheel_cat_trigger=trigger;
        frame(t,true,cx,cy);frame(t+240,true,cx,cy);
        CHECK(wheel.visible);
        frame(t+300,true,cx+50,cy);
        if(trigger)CHECK(!wheel.action);
        else CHECK(wheel.action==14 && wheel.action_item==25);
        frame(t+500,true,cx+50,cy);CHECK(!wheel.action);
        frame(t+510,false,cx+50,cy);
        if(trigger)CHECK(wheel.action==14 && wheel.action_item==25);
        else CHECK(!wheel.action);
        CHECK(!wheel.visible && !wheel.editing);
    }
    return 0;
}
