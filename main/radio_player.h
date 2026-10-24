#pragma once

#include <stdbool.h>

typedef struct {
    const char *name;
    const char *url;
} radio_station_t;

typedef enum {
    RADIO_EVENT_CONNECTING = 0,  // msg = station name
    RADIO_EVENT_STREAMING,       // msg = station name, stream established
    RADIO_EVENT_TRACK,           // msg = stream title from ICY metadata
    RADIO_EVENT_ERROR,           // msg = error description
    RADIO_EVENT_STOPPED,         // msg = ""
} radio_event_t;

typedef void (*radio_event_cb_t)(radio_event_t event, const char *msg, void *ctx);

void radio_init(const radio_station_t *stations, int count,
                radio_event_cb_t cb, void *ctx);

void radio_play(int index);      // switch to station and start playing
void radio_next(void);
void radio_prev(void);
void radio_stop(void);

/** Diagnostic: play a square wave through the I2S path (no network, no
 *  decoder). Stops current playback. freq 50-8000 Hz, duration 100-10000 ms. */
void radio_play_tone(int freq_hz, int duration_ms);

/** Diagnostic: drive the amplifier's SD pin from a gpio to test the module.
 *  mode: -1 = float (module default, (L+R)/2 mix), 0 = pull low (shutdown,
 *  the outputs must fall to ~0V), 1 = pull high (left channel only, 15 dB).
 *  The gpio idles as an input, so it never disturbs normal playback.
 *  Returns false when the feature is not configured. */
bool radio_set_amp_sd(int mode);

/** Update a station name and/or url at runtime (saved to NVS).
 *  Pass NULL to keep the current value. Reconnects if the station is active. */
void radio_set_station(int index, const char *name, const char *url);

/** Number of stations in the list (grows/shrinks with add/del). */
int radio_station_count(void);

/** Append a station (saved to NVS). name may be NULL -> derived from the url.
 *  Returns the new index, or -1 on error (bad url / list full). */
int radio_add_station(const char *name, const char *url);

/** Remove a station and shift the following ones down (saved to NVS). */
bool radio_del_station(int index);

/** I2S pin maps. The amplifier wiring can be changed at runtime and the
 *  choice is stored in NVS, so a firmware update cannot silence an
 *  amplifier that is already working. */
int  radio_i2s_pinmap_count(void);
int  radio_i2s_pinmap_current(void);
bool radio_i2s_pinmap_get(int idx, int *bck, int *ws, int *data);
/** Switch to pin map idx (stops playback, stores the choice in NVS). */
bool radio_set_i2s_pinmap(int idx);

const radio_station_t *radio_get_station(int index);   // NULL if out of range

void radio_set_volume(int volume);   // 0..100
int  radio_get_volume(void);

int         radio_current_index(void);
const char *radio_current_track(void);
bool        radio_is_playing(void);
