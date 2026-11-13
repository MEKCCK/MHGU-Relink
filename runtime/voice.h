// Voice chat (runtime/voice.c): this hunter's microphone to the other games in the local play network, and theirs to
// the speakers. 10 ms Opus frames (48 kHz mono S16, VoIP, 24 kbit/s, FEC); the packets ride the link's F_AUX messages,
// or in crossplay (a Ryujinx LDN lobby or a suyu-style room) a UDP broadcast on VOICE_UDP_PORT of the local play
// network (runtime/net/net.h: ldn_send_voice). [Voice] VoiceMode / MicVolume / VoiceVolume / NoiseSuppression are the
// settings (runtime/config.h).
//
// Built without Opus (no Opus headers, so a machine without the library): every call below does nothing, voice chat
// stays off, and voice_selftest() says -1.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void voice_tick(void);  // each frame, on the host's thread, after host_input_update (runtime/ui.c)
// A message for voice chat: an F_AUX message from another game (net/ldn.c), or in crossplay a datagram to
// VOICE_UDP_PORT (net/vsock.c). Another magic number's message is left alone; so is this game's own voice.
void voice_aux_received(const void *data, uint32_t len);
int voice_talking(void);                     // the players heard in the last frame
int voice_speakers(uint32_t *ids, int max);  // their network addresses, up to max; how many were written
bool voice_sending(void);                    // this game's microphone is going out
int voice_selftest(void);                    // a tone through Opus: the last packet's size, -1: no Opus / it failed
void voice_shutdown(void);                   // (at exit: the devices closed and the encoder freed)

#ifdef __cplusplus
}
#endif
