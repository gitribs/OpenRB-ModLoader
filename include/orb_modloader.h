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
} OrbModloaderPlayhead;

#define ORB_MODLOADER_MOD_NAME 65

typedef struct OrbModloaderSong {
    uint8_t p808[ORB_MODLOADER_P808];
    uint8_t p909[ORB_MODLOADER_P909];
    uint8_t shuffle;
    uint8_t mix808;
    uint8_t mix909;
    char mod_name[ORB_MODLOADER_MOD_NAME];
} OrbModloaderSong;

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

#ifdef __cplusplus
}
#endif

#endif
