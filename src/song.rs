//! Load and save ReBirth 2.x `.rbs` songs, keeping the TR-808, TR-909, and effect snapshot.

use crate::{Modloader, P808, P909, PATTERN_BYTES};
use orb_engine::load;
use orb_engine::song::{Delay, Distortion, DrumPattern, Pcf};
use orb_engine::tb303;
use orb_engine::tr808;
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
}

impl Default for SongInfo {
    fn default() -> Self {
        Self {
            p808: [0; P808],
            p909: [0; P909],
            shuffle: 0,
            mix808: 100,
            mix909: 100,
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

impl Modloader {
    /// Parse a ReBirth 2.x `.rbs` file, apply 808/909 patterns, and return panel values.
    /// 303s, compressor, and song-mode tracks are ignored. Mixer 808/909 levels and the
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
        Ok(info)
    }

    /// Write a 4.2 `.rbs` whose 808/909 match the current patterns and `info`. 303s are
    /// default parameters and rest patterns. Distortion, PCF, Delay, and 808/909 mixer
    /// routing come from `info` so a loaded song can round-trip its effect snapshot.
    pub fn save_song(&self, info: &SongInfo) -> Result<Vec<u8>, String> {
        let mut song = rbs::Song::empty();
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
        song.glob.song_mode = 0;

        song.mixer.master = 100;
        song.mixer.pcf_id = info.pcf_id;
        for ch in &mut song.mixer.channels {
            ch.enabled = 1;
            ch.level = 100;
            ch.pan = 64;
            ch.delay_send = 0;
            ch.dist = 0;
        }
        song.mixer.channels[2].level = info.mix808.min(127);
        song.mixer.channels[3].level = info.mix909.min(127);
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

        let tb = &tb303::DEFAULT_PARAMS[..9];
        song.tb303[0].params.copy_from_slice(tb);
        song.tb303[1].params.copy_from_slice(tb);

        song.tr808.kind = DrumKind::Tr808;
        song.tr808.params = info.p808.to_vec();
        song.tr808.patterns = patterns_from_bytes(&self.tr808.patterns[..]);
        song.tr909.kind = DrumKind::Tr909;
        song.tr909.params = info.p909.to_vec();
        song.tr909.patterns = patterns_from_bytes(&self.tr909.patterns[..]);

        song.write().map_err(|e| e.to_string())
    }
}
