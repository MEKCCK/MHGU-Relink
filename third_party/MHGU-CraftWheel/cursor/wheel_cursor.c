// Windows adapter for the PC Forge wheel. SDL window operations run on its UI thread.
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include "engine_module_api.h"

static int (*last_device)(void);
static void *(*game_window)(void);
static void *(*keyboard_focus)(void);
static bool (*relative_mode)(void *);
static bool (*window_size)(void *,int *,int *);
static bool (*pixel_size)(void *,int *,int *);
static void (*warp_mouse)(void *,float,float);
static bool (*run_main)(void (*)(void *),void *,bool);

static SRWLOCK lock=SRWLOCK_INIT;
static uintptr_t generation;
static bool pending, queued;
static unsigned center_x, center_y;

static void move_pointer(void *token){
    AcquireSRWLockExclusive(&lock);
    if((uintptr_t)token==generation && pending){
        queued=false;
        void *window=game_window();
        if(!window || keyboard_focus()!=window || last_device()!=1){
            pending=false;
        }else if(!relative_mode(window)){
            int w,h,pw,ph;
            if(window_size(window,&w,&h) && pixel_size(window,&pw,&ph) && pw>0 && ph>0 && w>0 && h>0){
                float sx=pw/1280.f, sy=ph/720.f, fit=sx<sy?sx:sy;
                float x=(pw*.5f+((float)center_x-640.f)*fit)*w/pw;
                float y=(ph*.5f+((float)center_y-360.f)*fit)*h/ph;
                warp_mouse(window,x,y);
            }
            pending=false;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

// r0 selects cancel (0), start (1) or poll (2). r1/r2 are 1280x720 canvas coordinates.
static void wheel_cursor(uint32_t *regs){
    uintptr_t ticket=0;
    AcquireSRWLockExclusive(&lock);
    if(regs[0]!=2){
        ++generation;
        queued=false;
        pending=regs[0]==1 && last_device()==1 && regs[1]<=1280 && regs[2]<=720;
        center_x=regs[1];
        center_y=regs[2];
    }
    if(pending && !queued){
        queued=true;
        ticket=generation;
    }
    regs[0]=pending;
    ReleaseSRWLockExclusive(&lock);
    if(ticket && !run_main(move_pointer,(void *)ticket,false)){
        AcquireSRWLockExclusive(&lock);
        if(ticket==generation)pending=queued=false;
        ReleaseSRWLockExclusive(&lock);
    }
}

MHGU_ENGINE_MODULE_EXPORT const uint32_t mhgu_engine_module_api=MHGU_ENGINE_MODULE_API;
MHGU_ENGINE_MODULE_EXPORT bool mhgu_engine_module_entry(const MhguHost *host,MhguEngineModule *out){
    HMODULE exe=GetModuleHandleW(NULL), sdl=GetModuleHandleW(L"SDL3.dll");
    if(!sdl)return false;
    uint32_t (*add)(const char *,void (*)(uint32_t *))=(void*)GetProcAddress(exe,"hle_register");
    last_device=(void*)GetProcAddress(exe,"host_input_last_device");
    game_window=(void*)GetProcAddress(exe,"ui_window");
    keyboard_focus=(void*)GetProcAddress(sdl,"SDL_GetKeyboardFocus");
    relative_mode=(void*)GetProcAddress(sdl,"SDL_GetWindowRelativeMouseMode");
    window_size=(void*)GetProcAddress(sdl,"SDL_GetWindowSize");
    pixel_size=(void*)GetProcAddress(sdl,"SDL_GetWindowSizeInPixels");
    warp_mouse=(void*)GetProcAddress(sdl,"SDL_WarpMouseInWindow");
    run_main=(void*)GetProcAddress(sdl,"SDL_RunOnMainThread");
    if(!add || !last_device || !game_window || !keyboard_focus || !relative_mode ||
       !window_size || !pixel_size || !warp_mouse || !run_main)return false;
    add("gu_wheel_cursor_v1",wheel_cursor);
    out->name="GU Wheel Cursor";
    host->log("Wheel cursor ready");
    return true;
}
