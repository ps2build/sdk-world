#ifndef AUDIO_SERVER_H
#define AUDIO_SERVER_H

#include <stdint.h>

/* Sets up the sound and channel state and starts the RPC server thread. Returns 0 or a negative error. */
int audio_server_start(void);

/* Reports AUDIO_STREAM_EVENT_* bits to the EE. Safe to call from interrupt context. */
void audio_server_signal_events(uint32_t events);

#endif
