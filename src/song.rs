//! Load and save ReBirth 2.x `.rbs` songs, keeping the TR-808, TR-909, GLOB, and song tracks.

use crate::{Modloader, P808, P909, PATTERN_BYTES};
use orb_engine::load;
use orb_engine::seq::Sequencer;
use orb_engine::song::{self, Delay, Device, Distortion, DrumPattern, Pcf, TICKS_PER_POS};
use orb_engine::tb303;
use orb_engine::tr808;
use orb_engine::tr909;
use rbs::{self, DrumKind, PatternDrum};

/// Panel state carried across the C boundary when a song is loaded or saved.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SongInfo {
    pub p808: [u8; P808],
    pub p909: [u8; P909],
    pub shuffle: u8,
    pub mix808: u8,
    pub mix909: u8,
    pub mix808_pan: u8,
    pub mix909_pan: u8,
    pub mod_name: [i8; 65],
    pub dist_enabled: u8,
    pub dist_amount: u8,
    pub dist_shape: u8,
    pub dist_808: u8,
    pub dist_909: u8,
    pub pcf_enabled: u8,
    pub pcf_frequency: u8,
    pub pcf_resonance: u8,
    pub pcf_amount: u8,
    pub pcf_wave: u8,
    pub pcf_decay: u8,
    pub pcf_mode: u8,
    pub pcf_id: u8,
    pub delay_enabled: u8,
    pub delay_steps: u8,
    pub delay_mode: u8,
    pub delay_feedback: u8,
    pub delay_pan: u8,
    pub delay_send_808: u8,
    pub delay_send_909: u8,
    pub song_mode: u8,
    pub loop_on: u8,
    /// bit 0: PCF was routed to the 808 on any TRAK; bit 1: to the 909.
    pub reserved: [u8; 3],
    pub tempo: u32,
    pub loop_start: u32,
    pub loop_end: u32,
}

impl Default for SongInfo {
    fn default() -> Self {
        Self {
            p808: [0; P808],
            p909: [0; P909],
            shuffle: 0,
            mix808: 100,
            mix909: 100,
            mix808_pan: 64,
            mix909_pan: 64,
            mod_name: [0; 65],
            dist_enabled: 0,
            dist_amount: 0,
            dist_shape: 0,
            dist_808: 0,
            dist_909: 0,
            pcf_enabled: 0,
            pcf_frequency: 0,
            pcf_resonance: 0,
            pcf_amount: 0,
            pcf_wave: 0,
            pcf_decay: 0,
            pcf_mode: 0,
            pcf_id: 0,
            delay_enabled: 0,
            delay_steps: 0,
            delay_mode: 0,
            delay_feedback: 0,
            delay_pan: 0,
            delay_send_808: 0,
            delay_send_909: 0,
            song_mode: 0,
            loop_on: 0,
            reserved: [0; 3],
            tempo: crate::DEFAULT_TEMPO_MILLI,
            loop_start: 0,
            loop_end: 4 * song::TICKS_PER_BAR,
        }
    }
}

fn param_byte(values: &[u8], index: usize) -> u8 {
    values.get(index).copied().unwrap_or(0)
}

fn write_params(dest: &mut rbs::Params, values: &[u8]) {
    let n = dest.values.len().min(values.len());
    dest.values[..n].copy_from_slice(&values[..n]);
}

fn copy_mod_name(src: &str, dest: &mut [i8; 65]) {
    dest.fill(0);
    let bytes = src.as_bytes();
    let n = bytes.len().min(64);
    for (i, &b) in bytes[..n].iter().enumerate() {
        dest[i] = b as i8;
    }
}

fn mod_name_of(info: &SongInfo) -> String {
    let bytes = info.mod_name.map(|c| c as u8);
    let n = bytes.iter().position(|&c| c == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..n]).into_owned()
}

const MIX_PCF_ID: u8 = 2;
const MIX_LEVEL_808: u8 = 18;
const MIX_PAN_808: u8 = 19;
const MIX_SEND_808: u8 = 20;
const MIX_DIST_808: u8 = 21;
const MIX_LEVEL_909: u8 = 24;
const MIX_PAN_909: u8 = 25;
const MIX_SEND_909: u8 = 26;
const MIX_DIST_909: u8 = 27;

fn last_at_tick0(events: &[song::TrackEvent], ctrl: u8) -> Option<u8> {
    events
        .iter()
        .filter(|e| e.ctrl == ctrl && e.tick == 0)
        .map(|e| e.value)
        .last()
}

fn last_nonzero(events: &[song::TrackEvent], ctrl: u8) -> Option<u8> {
    events
        .iter()
        .filter(|e| e.ctrl == ctrl && e.value != 0)
        .map(|e| e.value)
        .last()
}

fn overlay_u8(slot: &mut u8, events: &[song::TrackEvent], ctrl: u8) {
    if let Some(v) = last_at_tick0(events, ctrl) {
        *slot = v;
    }
}

fn set_rbs_tick0(track: &mut rbs::Track, ctrl: u8, value: u8) {
    if let Some(e) = track
        .events
        .iter_mut()
        .rev()
        .find(|e| e.pos == 0 && e.ctrl == ctrl)
    {
        e.value = value;
        return;
    }
    track.events.push(rbs::Event {
        pos: 0,
        ctrl,
        value,
    });
}

fn any_nonzero(events: &[song::TrackEvent], ctrl: u8) -> bool {
    events.iter().any(|e| e.ctrl == ctrl && e.value != 0)
}

fn any_eq(events: &[song::TrackEvent], ctrl: u8, value: u8) -> bool {
    events.iter().any(|e| e.ctrl == ctrl && e.value == value)
}

/// MIXR / DIST / PCF / DELY chunks are the pattern copy (often the state when song mode was
/// entered). Live start values and later routing live on TRAKs. Overlay tick 0 onto the
/// snapshot so Copy matches bar 1, and keep routing that appears anywhere in the song.
fn apply_fx_tracks(info: &mut SongInfo, tracks: &[Vec<song::TrackEvent>; 9]) {
    let mix = &tracks[Device::Mixer.id() as usize];
    let delay = &tracks[Device::Delay.id() as usize];
    let dist = &tracks[Device::Distortion.id() as usize];
    let pcf = &tracks[Device::Pcf.id() as usize];

    overlay_u8(&mut info.dist_enabled, dist, 0);
    overlay_u8(&mut info.dist_amount, dist, 1);
    overlay_u8(&mut info.dist_shape, dist, 2);
    overlay_u8(&mut info.pcf_enabled, pcf, 0);
    overlay_u8(&mut info.pcf_frequency, pcf, 1);
    overlay_u8(&mut info.pcf_resonance, pcf, 2);
    overlay_u8(&mut info.pcf_amount, pcf, 3);
    overlay_u8(&mut info.pcf_wave, pcf, 4);
    overlay_u8(&mut info.pcf_decay, pcf, 5);
    overlay_u8(&mut info.pcf_mode, pcf, 6);
    overlay_u8(&mut info.delay_enabled, delay, 0);
    overlay_u8(&mut info.delay_steps, delay, 1);
    overlay_u8(&mut info.delay_mode, delay, 2);
    overlay_u8(&mut info.delay_feedback, delay, 3);
    overlay_u8(&mut info.delay_pan, delay, 4);
    overlay_u8(&mut info.pcf_id, mix, MIX_PCF_ID);
    if let Some(v) = last_at_tick0(mix, MIX_DIST_808) {
        info.dist_808 = u8::from(v != 0);
    }
    if let Some(v) = last_at_tick0(mix, MIX_DIST_909) {
        info.dist_909 = u8::from(v != 0);
    }
    overlay_u8(&mut info.delay_send_808, mix, MIX_SEND_808);
    overlay_u8(&mut info.delay_send_909, mix, MIX_SEND_909);
    overlay_u8(&mut info.mix808, mix, MIX_LEVEL_808);
    overlay_u8(&mut info.mix909, mix, MIX_LEVEL_909);
    overlay_u8(&mut info.mix808_pan, mix, MIX_PAN_808);
    overlay_u8(&mut info.mix909_pan, mix, MIX_PAN_909);

    if any_nonzero(dist, 0) {
        info.dist_enabled = 1;
    }
    if any_nonzero(pcf, 0) {
        info.pcf_enabled = 1;
    }
    if any_nonzero(delay, 0) {
        info.delay_enabled = 1;
    }
    if any_nonzero(mix, MIX_DIST_808) {
        info.dist_808 = 1;
    }
    if any_nonzero(mix, MIX_DIST_909) {
        info.dist_909 = 1;
    }
    if info.delay_send_808 == 0 {
        if let Some(v) = last_nonzero(mix, MIX_SEND_808) {
            info.delay_send_808 = v;
        }
    }
    if info.delay_send_909 == 0 {
        if let Some(v) = last_nonzero(mix, MIX_SEND_909) {
            info.delay_send_909 = v;
        }
    }

    if info.pcf_id == 3 || any_eq(mix, MIX_PCF_ID, 3) {
        info.reserved[0] |= 1;
    }
    if info.pcf_id == 4 || any_eq(mix, MIX_PCF_ID, 4) {
        info.reserved[0] |= 2;
    }
}

/// 808/909 chunks are the pattern-mode copy. Tick-0 TRAKs are the live start. Overlay
/// Enable and Pattern so pattern mode matches bar 1 even when those never move later.
fn apply_drum_tracks(info: &mut SongInfo, tracks: &[Vec<song::TrackEvent>; 9]) {
    let t808 = &tracks[Device::Tr808.id() as usize];
    let t909 = &tracks[Device::Tr909.id() as usize];
    overlay_u8(&mut info.p808[tr808::ENABLED], t808, tr808::ENABLED as u8);
    overlay_u8(&mut info.p808[tr808::PATTERN], t808, tr808::PATTERN as u8);
    overlay_u8(&mut info.p909[tr909::ENABLED], t909, tr909::ENABLED as u8);
    overlay_u8(&mut info.p909[tr909::PATTERN], t909, tr909::PATTERN as u8);
}

fn dump_patterns(pats: &[DrumPattern; 32]) -> [u8; PATTERN_BYTES] {
    let mut buf = [0u8; PATTERN_BYTES];
    for (i, p) in pats.iter().enumerate() {
        let o = i * tr808::PATTERN_LEN;
        buf[o] = p.shuffle;
        buf[o + 1] = p.length.clamp(1, 16);
        for s in 0..16 {
            buf[o + 2 + 12 * s..o + 14 + 12 * s].copy_from_slice(&p.steps[s]);
        }
    }
    buf
}

fn patterns_from_bytes(buf: &[u8]) -> Vec<PatternDrum> {
    (0..tr808::PATTERNS)
        .map(|p| {
            let o = p * tr808::PATTERN_LEN;
            let steps = std::array::from_fn(|s| {
                let at = o + 2 + 12 * s;
                buf.get(at..at + 12)
                    .and_then(|b| b.try_into().ok())
                    .unwrap_or([0; 12])
            });
            PatternDrum {
                shuffle: buf.get(o).copied().unwrap_or(0),
                length: buf.get(o + 1).copied().unwrap_or(16).clamp(1, 16),
                steps,
            }
        })
        .collect()
}

pub fn seq_to_rbs_track(seq: &Sequencer, i: usize) -> rbs::Track {
    let mut track = rbs::Track::default();
    track.events = seq
        .events(i)
        .iter()
        .map(|e| rbs::Event {
            pos: (e.tick.max(0) as u32) / TICKS_PER_POS,
            ctrl: e.ctrl,
            value: e.value,
        })
        .collect();
    track
}

impl Modloader {
    /// Parse a ReBirth 2.x `.rbs` file, apply 808/909 patterns and song tracks, and return
    /// panel values. 303s and compressor still do not sound. Mixer 808/909 levels and the
    /// Distortion / PCF / Delay snapshot are kept for recall. On failure, [`Self::status`]
    /// holds the reason and patterns are left unchanged.
    pub fn load_song(&mut self, data: &[u8]) -> Result<SongInfo, String> {
        let file = rbs::Song::parse(data).map_err(|e| e.to_string())?;
        let song = load::convert(&file).map_err(|e| e.to_string())?;
        self.tr808
            .patterns
            .copy_from_slice(&dump_patterns(&song.tr808.patterns));
        self.tr909
            .patterns
            .copy_from_slice(&dump_patterns(&song.tr909.patterns));

        let mut info = SongInfo::default();
        info.p808.copy_from_slice(&song.tr808.params[..P808]);
        info.p909.copy_from_slice(&song.tr909.params[..P909]);
        info.shuffle = song.globals.shuffle;
        info.mix808 = song.mixer.channels[2].level;
        info.mix909 = song.mixer.channels[3].level;
        info.mix808_pan = song.mixer.channels[2].pan;
        info.mix909_pan = song.mixer.channels[3].pan;
        copy_mod_name(&song.mod_name, &mut info.mod_name);
        info.dist_enabled = param_byte(&song.dist.params, Distortion::ENABLED);
        info.dist_amount = param_byte(&song.dist.params, Distortion::AMOUNT);
        info.dist_shape = param_byte(&song.dist.params, Distortion::SHAPE);
        info.dist_808 = u8::from(song.mixer.channels[2].dist != 0);
        info.dist_909 = u8::from(song.mixer.channels[3].dist != 0);
        info.pcf_enabled = param_byte(&song.pcf.params, Pcf::ENABLED);
        info.pcf_frequency = param_byte(&song.pcf.params, Pcf::FREQUENCY);
        info.pcf_resonance = param_byte(&song.pcf.params, Pcf::RESONANCE);
        info.pcf_amount = param_byte(&song.pcf.params, Pcf::AMOUNT);
        info.pcf_wave = param_byte(&song.pcf.params, Pcf::WAVE);
        info.pcf_decay = param_byte(&song.pcf.params, Pcf::DECAY);
        info.pcf_mode = param_byte(&song.pcf.params, Pcf::MODE);
        info.pcf_id = song.mixer.pcf_id;
        info.delay_enabled = param_byte(&song.delay.params, Delay::ENABLED);
        info.delay_steps = param_byte(&song.delay.params, Delay::STEPS);
        info.delay_mode = param_byte(&song.delay.params, Delay::MODE);
        info.delay_feedback = param_byte(&song.delay.params, Delay::FEEDBACK);
        info.delay_pan = param_byte(&song.delay.params, Delay::PAN);
        info.delay_send_808 = song.mixer.channels[2].delay_send;
        info.delay_send_909 = song.mixer.channels[3].delay_send;
        info.song_mode = song.globals.song_mode;
        info.loop_on = song.globals.loop_on;
        info.tempo = song.globals.tempo;
        info.loop_start = song.globals.loop_start;
        info.loop_end = song.globals.loop_end;
        apply_fx_tracks(&mut info, &song.tracks);
        apply_drum_tracks(&mut info, &song.tracks);

        self.install_song(&file, &song.tracks, &song.globals, &info);
        Ok(info)
    }

    /// Write a 4.2 `.rbs` whose 808/909 match the current patterns and `info`. Tracks come
    /// from the sequencer (including 303/fx events kept from the last load). Distortion, PCF,
    /// Delay, and 808/909 mixer routing come from `info`.
    pub fn save_song(&self, info: &SongInfo) -> Result<Vec<u8>, String> {
        let mut song = self.kept_song.clone().unwrap_or_else(rbs::Song::empty);
        let name = mod_name_of(info);
        if name.chars().count() > 64 {
            return Err("mod name is too long".into());
        }
        song.glob.mod_name = if name.is_empty() {
            crate::STANDARD_NAME.into()
        } else {
            name
        };
        song.glob.shuffle = info.shuffle;
        song.glob.song_mode = info.song_mode;
        song.glob.loop_on = info.loop_on;
        song.glob.tempo = if info.tempo == 0 {
            crate::DEFAULT_TEMPO_MILLI
        } else {
            info.tempo
        };
        song.glob.loop_start = info.loop_start;
        song.glob.loop_end = info.loop_end.max(info.loop_start + song::TICKS_PER_BAR);

        song.mixer.pcf_id = info.pcf_id;
        if self.kept_song.is_none() {
            song.mixer.master = 100;
            for ch in &mut song.mixer.channels {
                ch.enabled = 1;
                ch.level = 100;
                ch.pan = 64;
                ch.delay_send = 0;
                ch.dist = 0;
            }
        }
        song.mixer.channels[2].level = info.mix808.min(127);
        song.mixer.channels[3].level = info.mix909.min(127);
        song.mixer.channels[2].pan = info.mix808_pan.min(127);
        song.mixer.channels[3].pan = info.mix909_pan.min(127);
        song.mixer.channels[2].dist = u8::from(info.dist_808 != 0);
        song.mixer.channels[3].dist = u8::from(info.dist_909 != 0);
        song.mixer.channels[2].delay_send = info.delay_send_808.min(127);
        song.mixer.channels[3].delay_send = info.delay_send_909.min(127);

        write_params(
            &mut song.dist,
            &[info.dist_enabled, info.dist_amount, info.dist_shape],
        );
        write_params(
            &mut song.pcf,
            &[
                info.pcf_enabled,
                info.pcf_frequency,
                info.pcf_resonance,
                info.pcf_amount,
                info.pcf_wave,
                info.pcf_decay,
                info.pcf_mode,
            ],
        );
        write_params(
            &mut song.delay,
            &[
                info.delay_enabled,
                info.delay_steps,
                info.delay_mode,
                info.delay_feedback,
                info.delay_pan,
            ],
        );

        if song.tb303[0].params.iter().all(|&b| b == 0) {
            let tb = &tb303::DEFAULT_PARAMS[..9];
            song.tb303[0].params.copy_from_slice(tb);
            song.tb303[1].params.copy_from_slice(tb);
        }

        song.tr808.kind = DrumKind::Tr808;
        song.tr808.params = info.p808.to_vec();
        song.tr808.patterns = patterns_from_bytes(&self.tr808.patterns[..]);
        song.tr909.kind = DrumKind::Tr909;
        song.tr909.params = info.p909.to_vec();
        song.tr909.patterns = patterns_from_bytes(&self.tr909.patterns[..]);

        for i in 0..9 {
            song.tracks[i] = seq_to_rbs_track(&self.seq, i);
        }
        let mix = &mut song.tracks[Device::Mixer.id() as usize];
        set_rbs_tick0(mix, MIX_LEVEL_808, info.mix808.min(127));
        set_rbs_tick0(mix, MIX_LEVEL_909, info.mix909.min(127));
        set_rbs_tick0(mix, MIX_PAN_808, info.mix808_pan.min(127));
        set_rbs_tick0(mix, MIX_PAN_909, info.mix909_pan.min(127));
        set_rbs_tick0(mix, MIX_DIST_808, u8::from(info.dist_808 != 0));
        set_rbs_tick0(mix, MIX_DIST_909, u8::from(info.dist_909 != 0));
        set_rbs_tick0(mix, MIX_PCF_ID, info.pcf_id);

        song.write().map_err(|e| e.to_string())
    }
}
