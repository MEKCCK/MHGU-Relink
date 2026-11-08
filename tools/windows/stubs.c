/* MinGW libgcc hooks the DLL does not use (the game code has no C++ exceptions): satisfy the loader. */
void *__end__;
void *___tls_start__;
void *___tls_end__;
void __register_frame_info(void) {}
void __deregister_frame_info(void) {}
