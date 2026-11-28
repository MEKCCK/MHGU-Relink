#include "forge/hook.h"
#include "forge/log.h"
#include "forge/nn/ro.h"
#include "forge/wrapper.h"

// nn::ro is process-global even when the Forge/ImGui instances are private.
// Install before sApp_run, hence before either Forge starts loading NROs.
static Hook s_hook;
static Result (*s_originalInitialize)();
static unsigned char s_mutex[FORGE_NNOS_MUTEX_SIZE] __attribute__((aligned(4)));
static bool s_initialized;

static Result initializeShared()
{
    forge_nnosLockMutex(s_mutex);
    Result result = 0;
    if (!s_initialized) {
        result = s_originalInitialize();
        if (result == 0) s_initialized = true;
        forge_log(3, "[WheelRO] process loader initialization result=0x%08X", result);
    } else {
        forge_log(3, "[WheelRO] reused initialized process loader");
    }
    forge_nnosUnlockMutex(s_mutex);
    return result;
}

extern "C" bool forge_sharedRo_install()
{
    forge_nnosInitializeMutex(s_mutex, false);
    s_hook = forge_hook_create((void*)nn::ro::Initialize, (void*)initializeShared, (void**)&s_originalInitialize);
    return s_originalInitialize != nullptr;
}
