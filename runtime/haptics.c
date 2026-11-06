// The phone's vibration (Android): its vibrator through com.mhgu.recomp.Haptics (android/app/src/main/java), by JNI
// from SDL's main thread.
//   haptics_tap     the phone's key click when a touch button goes down ([Touch] Vibration)
//   haptics_rumble  the game's rumble ([Controls] PhoneRumble), with or without a controller: one-shots of RUMBLE_MS
//                   renewed every RENEW_NS while it lasts (a stalled main thread cannot leave the phone buzzing), a new
//                   strength when it moves enough, stopped at once at 0. Nothing crosses JNI while nothing changes.
//   haptics_stop    the app going to the background (runtime/ui.c)
// Elsewhere these do nothing.
#include "haptics.h"

#ifdef __ANDROID__
#include <jni.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

#include "config.h"
#include "rt.h"

#define TAP_NS        25000000ull  // clicks closer together than this are one
#define RUMBLE_MS     120          // each one-shot's length
#define RENEW_NS      90000000ull  // renewed this often while the rumble lasts (before it runs out: no gap)
#define CHANGE_NS     20000000ull  // a new strength at most this often (the game sends one every 5 ms)
#define CHANGE_AMP    20           // and only when it moved more than this (amplitude 1..255)
#define TAP_RESUME_NS 20000000ull  // a click replaces the rumble on the motor: the rumble again this long after
#define FLOOR         1311         // the stronger motor under 2 %: off (a fade's tail, that the phone would buzz with)

// Haptics.java, looked for once on first use, by the activity's class loader (FindClass from a native thread only
// sees the system's classes). 0: not yet, 1: there, -1: not (no class or no vibrator: never tried again).
static int ready;
static jclass cls;
static jmethodID m_tap, m_rumble, m_stop;
static bool amp_control;  // (without it the phone picks the strength: only starts, renewals and stops are sent)

static uint64_t tap_ns;                 // the last click
static int rumble_amp;                  // the amplitude playing (1..255; 0: none)
static uint64_t rumble_ns, rumble_due;  // when it was sent, when it must be renewed

static void clear_exception(JNIEnv *env) {
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

// (in the caller's local frame)
static jclass find_class(JNIEnv *env) {
    jobject act = (jobject)SDL_GetAndroidActivity();
    if (!act || (*env)->ExceptionCheck(env)) return NULL;
    jmethodID get_loader = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, act), "getClassLoader",
                                               "()Ljava/lang/ClassLoader;");
    if (!get_loader) return NULL;
    jobject loader = (*env)->CallObjectMethod(env, act, get_loader);
    if (!loader || (*env)->ExceptionCheck(env)) return NULL;
    jmethodID load = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, loader), "loadClass",
                                         "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = load ? (*env)->NewStringUTF(env, "com.mhgu.recomp.Haptics") : NULL;
    if (!name) return NULL;
    jclass c = (jclass)(*env)->CallObjectMethod(env, loader, load, name);
    return (*env)->ExceptionCheck(env) ? NULL : c;
}

static JNIEnv *jni(void) {
    if (ready < 0) return NULL;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (!env || ready > 0) return env;
    ready = -1;
    if ((*env)->PushLocalFrame(env, 16) < 0) { clear_exception(env); return NULL; }
    jclass c = find_class(env);
    jint caps = 0;  // (Haptics.caps(): 0 no vibrator, 1 on / off only, 2 amplitude control)
    if (c) {
        m_tap = (*env)->GetStaticMethodID(env, c, "tap", "()V");
        m_rumble = m_tap ? (*env)->GetStaticMethodID(env, c, "rumble", "(II)V") : NULL;
        m_stop = m_rumble ? (*env)->GetStaticMethodID(env, c, "stop", "()V") : NULL;
        jmethodID m_caps = m_stop ? (*env)->GetStaticMethodID(env, c, "caps", "()I") : NULL;
        if (m_caps) caps = (*env)->CallStaticIntMethod(env, c, m_caps);
        if ((*env)->ExceptionCheck(env)) caps = 0;
        if (caps > 0) cls = (jclass)(*env)->NewGlobalRef(env, c);
    }
    clear_exception(env);
    (*env)->PopLocalFrame(env, NULL);
    if (!c) LOG("[haptics] com.mhgu.recomp.Haptics not found: the phone does not vibrate\n");
    else if (!cls) LOG("[haptics] no vibrator\n");
    else LOG("[haptics] vibrator%s\n", caps >= 2 ? ", amplitude control" : " (on / off only)");
    if (!cls) return NULL;
    amp_control = caps >= 2;
    ready = 1;
    return env;
}

static void call(JNIEnv *env, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    (*env)->CallStaticVoidMethodV(env, cls, m, ap);
    va_end(ap);
    clear_exception(env);
}

void haptics_tap(void) {
    if (!config_live_touch_vibration()) return;
    uint64_t now = plat_now_ns();
    if (tap_ns && now - tap_ns < TAP_NS) return;
    JNIEnv *env = jni();
    if (!env) return;
    tap_ns = now;
    call(env, m_tap);
    if (rumble_amp && rumble_due > now + TAP_RESUME_NS) rumble_due = now + TAP_RESUME_NS;
}

void haptics_rumble(uint32_t v) {
    uint32_t lo = v >> 16, hi = v & 0xFFFF, s = lo > hi ? lo : hi;
    bool on = s >= FLOOR && config_live_phone_rumble();
    if (!on && !rumble_amp) return;  // (nothing playing, nothing wanted: almost always, and no JNI)
    JNIEnv *env = jni();
    if (!env) return;
    if (!on) {
        call(env, m_stop);
        rumble_amp = 0;
        return;
    }
    // (a phone's small motor is hardly felt under ~60: a floor, and a curve that lifts the weak values)
    int amp = amp_control ? 50 + (int)(205.0f * powf(s / 65535.0f, 0.7f) + 0.5f) : 255;
    if (amp > 255) amp = 255;
    uint64_t now = plat_now_ns();
    bool change = !rumble_amp || (abs(amp - rumble_amp) > CHANGE_AMP && now - rumble_ns >= CHANGE_NS);
    if (!change && now < rumble_due) return;
    call(env, m_rumble, (jint)amp, (jint)RUMBLE_MS);
    rumble_amp = amp, rumble_ns = now, rumble_due = now + RENEW_NS;
}

void haptics_stop(void) {
    if (ready <= 0) return;  // (nothing was ever sent)
    JNIEnv *env = jni();
    if (env) call(env, m_stop);
    rumble_amp = 0;
}

#else
void haptics_tap(void) {}
void haptics_rumble(uint32_t v) { (void)v; }
void haptics_stop(void) {}
#endif
