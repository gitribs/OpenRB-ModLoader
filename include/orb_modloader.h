#ifndef ORB_MODLOADER_H
#define ORB_MODLOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_MODLOADER_AUX 22
#define ORB_MODLOADER_P808 29
#define ORB_MODLOADER_P909 30
#define ORB_MODLOADER_PATTERN_BYTES 6208

typedef struct OrbModloader OrbModloader;

#if defined(_MSC_VER)
#pragma pack(push, 1)
#endif
typedef struct OrbModloaderParams {
    uint8_t p808[ORB_MODLOADER_P808];
    uint8_t p909[ORB_MODLOADER_P909];
    uint8_t shuffle;
    uint8_t direct[ORB_MODLOADER_AUX];
    uint8_t bypass;
    uint8_t mix808;
    uint8_t mix909;
    uint8_t direct_mix808;
    uint8_t direct_mix909;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__ ((packed))
#endif
OrbModloaderParams;
#if defined(_MSC_VER)
#pragma pack(pop)
#endif

typedef struct OrbModloaderPlayhead {
    double bpm;
    double ppq;
    int32_t playing;
    int32_t host_driven;
} OrbModloaderPlayhead;

#define ORB_MODLOADER_MOD_NAME 65

typedef struct OrbModloaderSong {
    uint8_t p808[ORB_MODLOADER_P808];
    uint8_t p909[ORB_MODLOADER_P909];
    uint8_t shuffle;
    uint8_t mix808;
    uint8_t mix909;
    char mod_name[ORB_MODLOADER_MOD_NAME];
    uint8_t dist_enabled;
    uint8_t dist_amount;
    uint8_t dist_shape;
    uint8_t dist_808;
    uint8_t dist_909;
    uint8_t pcf_enabled;
    uint8_t pcf_frequency;
    uint8_t pcf_resonance;
    uint8_t pcf_amount;
    uint8_t pcf_wave;
    uint8_t pcf_decay;
    uint8_t pcf_mode;
    uint8_t pcf_id;
    uint8_t delay_enabled;
    uint8_t delay_steps;
    uint8_t delay_mode;
    uint8_t delay_feedback;
    uint8_t delay_pan;
    uint8_t delay_send_808;
    uint8_t delay_send_909;
    uint8_t song_mode;
    uint8_t loop_on;
    uint8_t reserved[3];
    uint32_t tempo;
    uint32_t loop_start;
    uint32_t loop_end;
} OrbModloaderSong;

typedef struct OrbModloaderTransport {
    int32_t song_mode;
    int32_t playing;
    int32_t recording;
    int32_t loop_on;
    int32_t bar;
    int32_t start;
    int32_t length;
    int32_t tempo_bpm;
    int32_t tick;
} OrbModloaderTransport;

typedef struct OrbModloaderTrackEvent {
    int32_t tick;
    uint8_t machine;
    uint8_t ctrl;
    uint8_t value;
    uint8_t reserved;
} OrbModloaderTrackEvent;

OrbModloader *orb_modloader_new(void);
void orb_modloader_free(OrbModloader *m);
void orb_modloader_reset(OrbModloader *m);
void orb_modloader_restart_clock(OrbModloader *m);
int32_t orb_modloader_latency(void);
void orb_modloader_process(OrbModloader *m,
                           float *main_l,
                           float *main_r,
                           float *mix808,
                           float *mix909,
                           float *const *aux,
                           int32_t nframes,
                           const OrbModloaderParams *params,
                           const OrbModloaderPlayhead *playhead);

int32_t orb_modloader_get_patterns(OrbModloader *m, int32_t machine, uint8_t *buf, int32_t len);
int32_t orb_modloader_set_patterns(OrbModloader *m, int32_t machine, const uint8_t *buf, int32_t len);
uint8_t orb_modloader_get_step(OrbModloader *m, int32_t machine, uint8_t pattern, uint8_t step, uint8_t col);
void orb_modloader_set_step(OrbModloader *m, int32_t machine, uint8_t pattern, uint8_t step, uint8_t col, uint8_t value);
uint8_t orb_modloader_get_pat_shuffle(OrbModloader *m, int32_t machine, uint8_t pattern);
void orb_modloader_set_pat_shuffle(OrbModloader *m, int32_t machine, uint8_t pattern, uint8_t on);
uint8_t orb_modloader_get_pat_length(OrbModloader *m, int32_t machine, uint8_t pattern);
void orb_modloader_set_pat_length(OrbModloader *m, int32_t machine, uint8_t pattern, uint8_t length);
int32_t orb_modloader_last_step(OrbModloader *m, int32_t machine);
void orb_modloader_trigger(OrbModloader *m, int32_t machine, uint8_t col, uint8_t velocity);

void orb_modloader_scan(OrbModloader *m, const char *folder);
int32_t orb_modloader_mod_count(OrbModloader *m);
int32_t orb_modloader_current_mod(OrbModloader *m);
void orb_modloader_mod_name(OrbModloader *m, int32_t index, char *buf, int32_t buflen);
void orb_modloader_mod_path(OrbModloader *m, int32_t index, char *buf, int32_t buflen);
void orb_modloader_mods_folder(OrbModloader *m, char *buf, int32_t buflen);
void orb_modloader_status(OrbModloader *m, char *buf, int32_t buflen);
void orb_modloader_dat_path(OrbModloader *m, char *buf, int32_t buflen);
void orb_modloader_set_dat_path(OrbModloader *m, const char *path);
int32_t orb_modloader_load(OrbModloader *m, int32_t index);
int32_t orb_modloader_splash_jpeg(OrbModloader *m, uint8_t *buf, int32_t len);
int32_t orb_modloader_load_song(OrbModloader *m, const uint8_t *data, int32_t len, OrbModloaderSong *out);
int32_t orb_modloader_save_song(OrbModloader *m, const OrbModloaderSong *in, uint8_t *buf, int32_t len);

void orb_modloader_reset_song(OrbModloader *m);
void orb_modloader_transport(OrbModloader *m, OrbModloaderTransport *out);
void orb_modloader_live_params(OrbModloader *m, OrbModloaderParams *out);
void orb_modloader_set_song_mode(OrbModloader *m, int32_t on);
void orb_modloader_play(OrbModloader *m);
void orb_modloader_stop(OrbModloader *m);
void orb_modloader_wind(OrbModloader *m, int32_t forward);
void orb_modloader_bar_step(OrbModloader *m, int32_t up);
void orb_modloader_loop_start_step(OrbModloader *m, int32_t delta_bars);
void orb_modloader_loop_length_step(OrbModloader *m, int32_t delta_bars);
void orb_modloader_set_loop_on(OrbModloader *m, int32_t on);
void orb_modloader_set_record(OrbModloader *m, int32_t on);
void orb_modloader_set_print(OrbModloader *m, int32_t on);
int32_t orb_modloader_copy_song_events(OrbModloader *m, OrbModloaderTrackEvent *buf, int32_t max);
void orb_modloader_init_from_pattern(OrbModloader *m, int32_t loop_only);
void orb_modloader_set_tempo_bpm(OrbModloader *m, int32_t bpm);
uint32_t orb_modloader_tempo_milli(OrbModloader *m);
void orb_modloader_record_param(OrbModloader *m, int32_t machine, uint8_t ctrl, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
