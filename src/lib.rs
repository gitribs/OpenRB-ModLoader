//! TR-808 and TR-909 wrapper around [`orb_engine`], for the OpenRB Modloader plugin.
//!
//! Both machines share one [`ProcessState`] and one [`SampleSet`]. Audio is processed in
//! 64-sample blocks at 44100 Hz. Output is delayed by one block so a host sees a constant
//! 64-sample latency. The C API takes (and returns) buffers already at 44100 Hz; resampling
//! is the JUCE plugin's job.
//!
//! The clock is ReBirth's song position. Pattern mode follows the host playhead and loops the
//! selected pattern. Song mode uses [`Sequencer`]: pattern chaining, loop, record. In a plugin
//! the host drives play, stop, and position; standalone keeps its own transport. Shuffle uses
//! the global knob when that pattern's shuffle flag is on.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::Arc;

mod fwd;
mod iso;
mod song;

use fwd::{Fwd808, Fwd909};
use orb_engine::frame::{BLOCK, Block, ChannelDevice, Devices, GUARD, SongDevice};
use orb_engine::process::ProcessState;
use orb_engine::samples::{Retired, SampleSet};
use orb_engine::seq::{self, Sequencer, TEMPO_STEP};
use orb_engine::song::{self as orbsong, Device, PPQ};
use orb_engine::tables;
use orb_engine::{tr808, tr909};
use rbm::{KeyTable, Rbm, RbmError, standard};
use rbs;
use song::SongInfo;

/// Engine (and original) sample rate.
pub const ENGINE_RATE: f64 = 44100.0;
/// Fallback tempo when the host has none, as BPM*1000.
pub const DEFAULT_TEMPO_MILLI: u32 = 120_000;
/// Instrument aux buses: 11 per machine. Machine Mix buses are separate.
pub const AUX: usize = 22;
/// Controllers 0..=28 of the 808.
pub const P808: usize = 29;
/// Controllers 0..=29 of the 909 (RBS / panel order).
pub const P909: usize = 30;
/// One machine's pattern dump: 32 × 194.
pub const PATTERN_BYTES: usize = tr808::PATTERNS * tr808::PATTERN_LEN;
/// Default ReBirth Mods folder on macOS.
pub const DEFAULT_MODS_DIR: &str = "/Library/Application Support/Propellerhead/Rebirth RB-338/Mods";
/// Mixer controller for the 808 channel level (`5 + 6*2 + 1`).
const MIX_808_LEVEL: u8 = 18;
/// Mixer controller for the 909 channel level (`5 + 6*3 + 1`).
const MIX_909_LEVEL: u8 = 24;
/// Ticks per bar.
const BAR: i32 = orbsong::TICKS_PER_BAR as i32;
/// Start of bar 999.
const MAX_POS: i32 = 766_464;
const MAX_END: i32 = 767_232;

const STANDARD_NAME: &str = standard::NAME;

/// Panel parameters applied at the start of each 64-sample block.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct Params {
    pub p808: [u8; P808],
    pub p909: [u8; P909],
    pub shuffle: u8,
    pub direct: [u8; AUX],
    pub bypass: u8,
    /// Mixer channel level of the 808 (ReBirth MIXR channel 2, 0..=127, default 100).
    pub mix808: u8,
    /// Mixer channel level of the 909 (ReBirth MIXR channel 3, 0..=127, default 100).
    pub mix909: u8,
    /// Omit the 808 Mix from the main Output (3/4 still has it).
    pub direct_mix808: u8,
    /// Omit the 909 Mix from the main Output (5/6 still has it).
    pub direct_mix909: u8,
}

impl Default for Params {
    fn default() -> Self {
        let mut p808 = [0u8; P808];
        p808.copy_from_slice(&tr808::DEFAULT_PARAMS[..P808]);
        let mut p909 = [0u8; P909];
        p909.copy_from_slice(&tr909::DEFAULT_PARAMS[..P909]);
        Self {
            p808,
            p909,
            shuffle: 64,
            direct: [0; AUX],
            bypass: 0,
            mix808: 100,
            mix909: 100,
            direct_mix808: 0,
            direct_mix909: 0,
        }
    }
}

/// Host playhead at the first 44100 Hz frame of this callback.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Playhead {
    pub bpm: f64,
    pub ppq: f64,
    pub playing: bool,
    /// Plugin hosts set this so song mode follows play, stop, and locate. Standalone leaves it off.
    pub host_driven: bool,
}

/// C layout of [`Playhead`]: `playing` is a 32-bit flag, not a Rust `bool`.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct PlayheadC {
    pub bpm: f64,
    pub ppq: f64,
    pub playing: i32,
    pub host_driven: i32,
}

impl From<PlayheadC> for Playhead {
    fn from(p: PlayheadC) -> Self {
        Self {
            bpm: p.bpm,
            ppq: p.ppq,
            playing: p.playing != 0,
            host_driven: p.host_driven != 0,
        }
    }
}

/// Song-mode transport snapshot for the panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(C)]
pub struct Transport {
    pub song_mode: i32,
    pub playing: i32,
    pub recording: i32,
    pub loop_on: i32,
    pub bar: i32,
    pub start: i32,
    pub length: i32,
    pub tempo_bpm: i32,
    pub tick: i32,
}

impl Default for Playhead {
    fn default() -> Self {
        Self {
            bpm: 120.0,
            ppq: 0.0,
            playing: false,
            host_driven: false,
        }
    }
}

fn host_pos(ppq: f64, bpm: f64, k: usize) -> f64 {
    ppq * PPQ as f64 + (k as f64 + 1.0) * tempo_milli(bpm) as f64 * TEMPO_STEP
}

fn block_inc(bpm: f64) -> f64 {
    tempo_milli(bpm) as f64 * TEMPO_STEP
}

fn quarters_in(n: usize, bpm: f64) -> f64 {
    let bpm = if !bpm.is_finite() || bpm <= 0.0 {
        120.0
    } else {
        bpm
    };
    n as f64 / ENGINE_RATE * bpm / 60.0
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ClockMode {
    Hold,
    Follow,
    FreeRun,
}

fn tempo_milli(bpm: f64) -> u32 {
    if !bpm.is_finite() || bpm <= 0.0 {
        DEFAULT_TEMPO_MILLI
    } else {
        (bpm * 1000.0).round() as u32
    }
}

fn copy_cstr(src: &str, buf: *mut i8, buflen: i32) {
    if buf.is_null() || buflen <= 0 {
        return;
    }
    let bytes = src.as_bytes();
    let n = (buflen as usize - 1).min(bytes.len());
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, n);
        *buf.add(n) = 0;
    }
}

fn byte_order(a: &Path, b: &Path) -> std::cmp::Ordering {
    let key = |p: &Path| p.file_name().map(|f| f.as_encoded_bytes().to_vec());
    key(a).cmp(&key(b)).then_with(|| a.cmp(b))
}

/// ReBirth mixer keep/take/ramp (`dbl_10151F48`, `dbl_10151F50`, `dbl_10151F58`).
const MIX_KEEP: f64 = f64::from_bits(0x3FED_C28F_5C28_F5C3);
const MIX_TAKE: f64 = f64::from_bits(0x3FB1_EB85_1EB8_51EC);
const MIX_RAMP: f64 = f64::from_bits(0x3FA0_0000_0000_0000);

fn mixer_target(level: u8, curve: &[f32; 128]) -> f32 {
    let i = (level as usize).min(127);
    (GUARD as f64 + curve[i] as f64) as f32
}

fn copy_delay(dst: &mut [f32], src: &[f32; BLOCK], i: usize, fill: usize, take: usize) {
    if i + take <= dst.len() {
        dst[i..i + take].copy_from_slice(&src[fill..fill + take]);
    }
}

/// Mixer channel gain: `T[level] + 1e-6` at pan centre, smoothed like `sub_10013444`.
fn mix_channel(src: &[f32; BLOCK], old: f32, target: f32) -> (f32, [f32; BLOCK]) {
    let old_d = old as f64;
    let new_d = old_d * MIX_KEEP + target as f64 * MIX_TAKE;
    let stored = new_d as f32;
    let dl = (new_d - old_d) * MIX_RAMP;
    let mut out = [0.0f32; BLOCK];
    let mut g = old_d;
    for i in (0..BLOCK).step_by(2) {
        for j in i..i + 2 {
            out[j] = (src[j] as f64 * g) as f32;
        }
        g += dl;
    }
    (stored, out)
}

#[derive(Clone)]
struct ModEntry {
    /// Empty for the built-in Standard ReBirth item.
    path: PathBuf,
    name: String,
}

fn find_dat(explicit: Option<&Path>, mods_dir: &Path) -> Option<PathBuf> {
    if let Some(p) = explicit {
        return p.is_file().then(|| p.to_path_buf());
    }
    if let Some(p) = std::env::var_os("ORB_RB20FUL")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        && p.is_file()
    {
        return Some(p);
    }
    let mut dirs = vec![mods_dir.to_path_buf()];
    if let Some(parent) = mods_dir.parent() {
        dirs.push(parent.to_path_buf());
    }
    for d in dirs {
        for name in ["RB20FUL.DAT", "rb20ful.dat", "Rb20ful.dat"] {
            let p = d.join(name);
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

/// Big-endian `DAT_MAGIC` (`0x03E1F11E`) at the start of `RB20FUL.DAT`.
const DAT_MAGIC_BE: [u8; 4] = [0x03, 0xE1, 0xF1, 0x1E];

enum KeySource {
    Dat(File),
    Iso(iso::Window),
}

impl Read for KeySource {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        match self {
            Self::Dat(f) => f.read(buf),
            Self::Iso(w) => w.read(buf),
        }
    }
}

impl Seek for KeySource {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        match self {
            Self::Dat(f) => f.seek(to),
            Self::Iso(w) => w.seek(to),
        }
    }
}

/// Open a loose `RB20FUL.DAT` or an ISO 9660 image that contains one. The DAT body
/// is read in place; nothing is copied out of the image.
fn open_key(path: &Path) -> Result<KeySource, String> {
    let mut file = File::open(path).map_err(|_| "needs DAT/ISO".to_string())?;
    let mut head = [0u8; 4];
    if file.read_exact(&mut head).is_ok() && head == DAT_MAGIC_BE {
        file.seek(SeekFrom::Start(0))
            .map_err(|_| "needs DAT/ISO".to_string())?;
        return Ok(KeySource::Dat(file));
    }
    drop(file);
    iso::open_dat(path)
        .map(KeySource::Iso)
        .map_err(|_| "needs DAT/ISO".to_string())
}

struct LoadedKit {
    set: Arc<SampleSet>,
    splash: Vec<u8>,
}

fn splash_jpeg(rbm: &Rbm, table: &KeyTable) -> Vec<u8> {
    rbm.file_by_name("splash.jpg", table)
        .ok()
        .filter(|b| b.starts_with(&[0xFF, 0xD8]))
        .unwrap_or_default()
}

fn builtin_splash() -> Vec<u8> {
    standard::builtin_file("splash.jpg")
        .filter(|b| b.starts_with(&[0xFF, 0xD8]))
        .unwrap_or_default()
}

fn load_kit(path: &Path, dat: Option<&Path>, mods_dir: &Path) -> Result<LoadedKit, String> {
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    let rbm = Rbm::parse(bytes).map_err(|e| e.to_string())?;
    let builtin = standard::builtin_samples();
    let table = match KeyTable::embedded(&rbm).filter(|t| rbm.slots_covered(t)) {
        Some(t) => t,
        None => {
            let dat_path = find_dat(dat, mods_dir).ok_or_else(|| "needs DAT/ISO".to_string())?;
            let mut key = open_key(&dat_path)?;
            KeyTable::read(&rbm, &mut key).map_err(|e| match e {
                RbmError::KeyFile(_) | RbmError::Io(_) => "needs DAT/ISO".to_string(),
                other => other.to_string(),
            })?
        }
    };
    match rbm.load_samples(&table, &builtin) {
        Ok(loaded) => Ok(LoadedKit {
            set: Arc::new(loaded.set),
            splash: splash_jpeg(&rbm, &table),
        }),
        Err(RbmError::KeyNotCovered(_)) => Err("needs DAT/ISO".to_string()),
        Err(RbmError::WrongKey(_)) => Err("bad key".to_string()),
        Err(e) => Err(e.to_string()),
    }
}

/// Both drum machines, one clock, a 64-sample output delay, and the scanned mod list.
pub struct Modloader {
    tr808: tr808::Tr808,
    tr909: tr909::Tr909,
    seq: Sequencer,
    devices: Devices,
    bound_at: usize,
    last_params: Params,
    kept_song: Option<rbs::Song>,
    retired: Option<Retired>,
    work_main: Block,
    work_mix808: Block,
    work_mix909: Block,
    work_aux: [Block; AUX],
    delay_main: Block,
    delay_mix808: Block,
    delay_mix909: Block,
    delay_aux: [Block; AUX],
    fill: usize,
    pos: f64,
    was_playing: bool,
    last_ppq: Option<f64>,
    held_tick: i32,
    last_tick: Option<i32>,
    blocks_processed: u64,
    mix_level: [f32; 128],
    gain_808: f32,
    gain_909: f32,
    mix_primed: bool,
    mods: Vec<ModEntry>,
    mods_dir: PathBuf,
    dat_path: Option<PathBuf>,
    current: usize,
    status: String,
    /// `splash.jpg` of the kit that is sounding, or empty if the mod has none.
    splash: Vec<u8>,
    /// MIDI hits queued on the audio thread until the next 64-sample engine block.
    pending_midi: [(i32, u8, u8); 32],
    pending_midi_n: usize,
}

impl Default for Modloader {
    fn default() -> Self {
        Self::new()
    }
}

impl Modloader {
    pub fn new() -> Self {
        let samples = standard::builtin_samples();
        let process = ProcessState::new();
        let mods_dir = std::env::var_os("ORB_MOD_DIR")
            .filter(|v| !v.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(DEFAULT_MODS_DIR));
        let mix_level = tables::level_curve();
        let mix_gain = mixer_target(100, &mix_level);
        let mut m = Self {
            tr808: tr808::Tr808::new(samples.clone(), process.clone()),
            tr909: tr909::Tr909::new(samples, process),
            seq: Sequencer::default(),
            devices: Devices::placeholders(),
            bound_at: 0,
            last_params: Params::default(),
            kept_song: None,
            retired: None,
            work_main: [0.0; BLOCK],
            work_mix808: [0.0; BLOCK],
            work_mix909: [0.0; BLOCK],
            work_aux: [[0.0; BLOCK]; AUX],
            delay_main: [0.0; BLOCK],
            delay_mix808: [0.0; BLOCK],
            delay_mix909: [0.0; BLOCK],
            delay_aux: [[0.0; BLOCK]; AUX],
            fill: 0,
            pos: 0.0,
            was_playing: false,
            last_ppq: None,
            held_tick: 0,
            last_tick: None,
            blocks_processed: 0,
            mix_level,
            gain_808: mix_gain,
            gain_909: mix_gain,
            mix_primed: false,
            mods: Vec::new(),
            mods_dir,
            dat_path: None,
            current: 0,
            status: String::new(),
            splash: builtin_splash(),
            pending_midi: [(0, 0, 0); 32],
            pending_midi_n: 0,
        };
        m.ensure_bound();
        m.init_device_tracks();
        m.scan_dir();
        m
    }

    /// Clear the output delay and clock. Patterns, the loaded kit, and the DAT path stay;
    /// hosts call this on bus connect and on prepareToPlay.
    pub fn reset(&mut self) {
        self.work_main = [0.0; BLOCK];
        self.work_mix808 = [0.0; BLOCK];
        self.work_mix909 = [0.0; BLOCK];
        self.work_aux = [[0.0; BLOCK]; AUX];
        self.delay_main = [0.0; BLOCK];
        self.delay_mix808 = [0.0; BLOCK];
        self.delay_mix909 = [0.0; BLOCK];
        self.delay_aux = [[0.0; BLOCK]; AUX];
        self.fill = 0;
        self.pending_midi_n = 0;
        self.restart_clock();
        self.blocks_processed = 0;
    }

    pub fn restart_clock(&mut self) {
        self.pos = 0.0;
        self.was_playing = false;
        self.last_ppq = None;
        self.held_tick = 0;
        self.last_tick = None;
    }

    pub fn latency() -> usize {
        BLOCK
    }

    pub fn last_tick(&self) -> Option<i32> {
        self.last_tick
    }

    pub fn blocks_processed(&self) -> u64 {
        self.blocks_processed
    }

    pub fn process(
        &mut self,
        main_l: &mut [f32],
        main_r: &mut [f32],
        mix808: &mut [f32],
        mix909: &mut [f32],
        aux: &mut [&mut [f32]],
        params: Params,
        playhead: Playhead,
    ) {
        let n = main_l.len().min(main_r.len());
        let tempo = tempo_milli(playhead.bpm);
        let song_mode = self.song_mode();
        let host_song = song_mode && playhead.host_driven;
        let mode = if host_song {
            self.update_clock_mode(playhead, n)
        } else if song_mode {
            ClockMode::Hold
        } else {
            self.begin_clock(playhead, n)
        };
        let mut i = 0;
        let mut k = 0usize;

        while i < n {
            let take = (n - i).min(BLOCK - self.fill);
            main_l[i..i + take].copy_from_slice(&self.delay_main[self.fill..self.fill + take]);
            main_r[i..i + take].copy_from_slice(&self.delay_main[self.fill..self.fill + take]);
            copy_delay(mix808, &self.delay_mix808, i, self.fill, take);
            copy_delay(mix909, &self.delay_mix909, i, self.fill, take);
            for (ch, bus) in aux.iter_mut().enumerate().take(AUX) {
                if i + take <= bus.len() {
                    bus[i..i + take]
                        .copy_from_slice(&self.delay_aux[ch][self.fill..self.fill + take]);
                }
            }
            self.fill += take;
            i += take;

            if self.fill == BLOCK {
                self.ensure_bound();
                let (tick, stepped) = if song_mode {
                    self.seq.set_tempo(tempo);
                    self.seq.set_shuffle(params.shuffle);
                    self.apply_panel_song(&params);
                    if host_song {
                        self.drive_song_host(playhead, k, mode);
                    } else if self.seq.globals.play != 0 {
                        self.with_devices(|seq, dev| {
                            seq.tick(dev);
                        });
                    }
                    let playing = self.seq.globals.play != 0;
                    (self.seq.tick_pos(), playing)
                } else {
                    let tick = self.take_tick(playhead, k, mode);
                    self.apply_params(params);
                    let shuffled = seq::shuffled(tick, params.shuffle);
                    self.tr808.step(tick, shuffled, tempo as i32);
                    self.tr909.step(tick, shuffled, tempo as i32);
                    (tick, mode != ClockMode::Hold)
                };
                self.drain_midi();

                let mut v808 = [[0.0f32; BLOCK]; tr808::VOICES];
                let mut v909 = [[0.0f32; BLOCK]; tr909::VOICES];
                let mut mix808_sum = [0.0f32; BLOCK];
                let mut mix909_sum = [0.0f32; BLOCK];
                let mut d808 = [false; tr808::VOICES];
                let mut d909 = [false; tr909::VOICES];
                for i in 0..tr808::VOICES {
                    d808[i] = params.direct[i] != 0;
                    d909[i] = params.direct[tr808::VOICES + i] != 0;
                }
                self.tr808.process_mix(&mut mix808_sum, &mut v808, &d808);
                self.tr909.process_mix(&mut mix909_sum, &mut v909, &d909);

                let mix808_level = if song_mode {
                    self.devices.mixer.live.channels[2].level
                } else {
                    params.mix808
                };
                let mix909_level = if song_mode {
                    self.devices.mixer.live.channels[3].level
                } else {
                    params.mix909
                };
                let t808 = mixer_target(mix808_level, &self.mix_level);
                let t909 = mixer_target(mix909_level, &self.mix_level);
                if !self.mix_primed {
                    self.gain_808 = t808;
                    self.gain_909 = t909;
                    self.mix_primed = true;
                }
                let (g808, scaled808) = mix_channel(&mix808_sum, self.gain_808, t808);
                let (g909, scaled909) = mix_channel(&mix909_sum, self.gain_909, t909);
                self.gain_808 = g808;
                self.gain_909 = g909;

                if params.bypass == 0 {
                    for s in 0..BLOCK {
                        self.work_main[s] = 0.0;
                        if params.direct_mix808 == 0 {
                            self.work_main[s] += scaled808[s];
                        }
                        if params.direct_mix909 == 0 {
                            self.work_main[s] += scaled909[s];
                        }
                    }
                    self.work_mix808 = scaled808;
                    self.work_mix909 = scaled909;
                    for (i, v) in v808.iter().enumerate() {
                        self.work_aux[i] = *v;
                    }
                    for (i, v) in v909.iter().enumerate() {
                        self.work_aux[tr808::VOICES + i] = *v;
                    }
                } else {
                    self.work_main = [0.0; BLOCK];
                    self.work_mix808 = [0.0; BLOCK];
                    self.work_mix909 = [0.0; BLOCK];
                    self.work_aux = [[0.0; BLOCK]; AUX];
                }

                if stepped {
                    self.held_tick = tick;
                }
                self.last_tick = Some(tick);
                std::mem::swap(&mut self.work_main, &mut self.delay_main);
                std::mem::swap(&mut self.work_mix808, &mut self.delay_mix808);
                std::mem::swap(&mut self.work_mix909, &mut self.delay_mix909);
                std::mem::swap(&mut self.work_aux, &mut self.delay_aux);
                self.fill = 0;
                self.blocks_processed += 1;
                k += 1;
            }
        }
    }

    fn update_clock_mode(&mut self, playhead: Playhead, n: usize) -> ClockMode {
        if n == 0 || !playhead.playing {
            self.was_playing = false;
            return ClockMode::Hold;
        }
        let mode = if !self.was_playing {
            ClockMode::Follow
        } else if let Some(prev) = self.last_ppq {
            let expected = quarters_in(n, playhead.bpm);
            let delta = (playhead.ppq - prev).abs();
            if expected > 0.0 && delta < expected * 0.5 {
                ClockMode::FreeRun
            } else {
                ClockMode::Follow
            }
        } else {
            ClockMode::Follow
        };
        self.was_playing = true;
        self.last_ppq = Some(playhead.ppq);
        mode
    }

    fn begin_clock(&mut self, playhead: Playhead, n: usize) -> ClockMode {
        let starting = n > 0 && playhead.playing && !self.was_playing;
        let mode = self.update_clock_mode(playhead, n);
        if starting {
            let tick = seq::trunc(host_pos(playhead.ppq, playhead.bpm, 0));
            self.tr808.start(tick);
            self.tr909.start(tick);
            self.tr808.relocate(tick, tick);
            self.tr909.relocate(tick, tick);
        }
        mode
    }

    fn drive_song_host(&mut self, playhead: Playhead, k: usize, mode: ClockMode) {
        match mode {
            ClockMode::Hold => {
                if k != 0 {
                    return;
                }
                self.with_devices(|seq, dev| {
                    if seq.globals.byte2 != 0 {
                        seq.set_record(0, dev);
                    }
                    if seq.globals.play != 0 {
                        seq.halt(dev);
                    }
                    let pos = host_pos(playhead.ppq, playhead.bpm, 0).clamp(0.0, MAX_POS as f64);
                    seq.follow_host(pos, dev);
                });
            }
            ClockMode::Follow => {
                let pos = host_pos(playhead.ppq, playhead.bpm, k).clamp(0.0, MAX_POS as f64);
                self.with_devices(|seq, dev| {
                    seq.follow_host(pos, dev);
                    seq.play(dev);
                    let t = seq.tick_pos();
                    let sp = seq::shuffled(t, seq.globals.shuffle);
                    let tempo = seq.globals.tempo as i32;
                    dev.for_each(|d| d.step(t, sp, tempo));
                });
            }
            ClockMode::FreeRun => {
                let inc = block_inc(playhead.bpm);
                self.with_devices(|seq, dev| {
                    if seq.globals.play == 0 {
                        seq.play(dev);
                    }
                    let pos = (seq.pos + inc).clamp(0.0, MAX_POS as f64);
                    seq.follow_host(pos, dev);
                    let t = seq.tick_pos();
                    let sp = seq::shuffled(t, seq.globals.shuffle);
                    let tempo = seq.globals.tempo as i32;
                    dev.for_each(|d| d.step(t, sp, tempo));
                });
            }
        }
    }

    fn take_tick(&mut self, playhead: Playhead, k: usize, mode: ClockMode) -> i32 {
        match mode {
            ClockMode::Hold => self.held_tick,
            ClockMode::Follow => {
                self.pos = host_pos(playhead.ppq, playhead.bpm, k);
                seq::trunc(self.pos)
            }
            ClockMode::FreeRun => {
                self.pos += block_inc(playhead.bpm);
                seq::trunc(self.pos)
            }
        }
    }

    fn apply_params(&mut self, p: Params) {
        for (i, &v) in p.p808.iter().enumerate() {
            self.tr808.set_param(i as u8, v);
        }
        for (i, &v) in p.p909.iter().enumerate() {
            self.tr909.set_param(i as u8, v);
        }
        self.last_params.p808 = p.p808;
        self.last_params.p909 = p.p909;
        self.last_params.mix808 = p.mix808;
        self.last_params.mix909 = p.mix909;
        self.last_params.shuffle = p.shuffle;
    }

    fn apply_panel_song(&mut self, p: &Params) {
        for i in 0..P808 {
            if p.p808[i] != self.last_params.p808[i] {
                self.tr808.set_param(i as u8, p.p808[i]);
                self.seq.record_hook(Device::Tr808, i as u8, p.p808[i]);
            }
        }
        for i in 0..P909 {
            if p.p909[i] != self.last_params.p909[i] {
                self.tr909.set_param(i as u8, p.p909[i]);
                self.seq.record_hook(Device::Tr909, i as u8, p.p909[i]);
            }
        }
        if p.mix808 != self.last_params.mix808 {
            self.devices.mixer.set_param(MIX_808_LEVEL, p.mix808);
            self.seq.record_hook(Device::Mixer, MIX_808_LEVEL, p.mix808);
        }
        if p.mix909 != self.last_params.mix909 {
            self.devices.mixer.set_param(MIX_909_LEVEL, p.mix909);
            self.seq.record_hook(Device::Mixer, MIX_909_LEVEL, p.mix909);
        }
        self.last_params.p808 = p.p808;
        self.last_params.p909 = p.p909;
        self.last_params.mix808 = p.mix808;
        self.last_params.mix909 = p.mix909;
        self.last_params.shuffle = p.shuffle;
    }

    fn rbs_909_live(&self) -> [u8; P909] {
        let mut p = [0u8; P909];
        for c in 0..P909 {
            p[c] = self.tr909.live[tr909::internal_index(c)];
        }
        p
    }

    pub fn live_params(&self) -> Params {
        let mut p = Params::default();
        p.p808.copy_from_slice(&self.tr808.live[..P808]);
        p.p909 = self.rbs_909_live();
        p.shuffle = self.seq.globals.shuffle;
        p.mix808 = self.devices.mixer.live.channels[2].level;
        p.mix909 = self.devices.mixer.live.channels[3].level;
        p
    }

    pub fn song_mode(&self) -> bool {
        self.seq.globals.song_mode != 0
    }

    fn ensure_bound(&mut self) {
        let addr = std::ptr::from_ref(&self.tr808) as usize;
        if self.bound_at == addr {
            return;
        }
        let p808 = NonNull::from(&mut self.tr808);
        let p909 = NonNull::from(&mut self.tr909);
        self.devices.channels[2] = Box::new(Fwd808(p808));
        self.devices.channels[3] = Box::new(Fwd909(p909));
        self.bound_at = std::ptr::from_ref(&self.tr808) as usize;
    }

    fn with_devices<R>(&mut self, f: impl FnOnce(&mut Sequencer, &mut Devices) -> R) -> R {
        self.ensure_bound();
        f(&mut self.seq, &mut self.devices)
    }

    fn init_device_tracks(&mut self) {
        self.ensure_bound();
        SongDevice::init_track(&mut self.tr808, &mut self.seq.track_mut(Device::Tr808.id() as usize));
        SongDevice::init_track(&mut self.tr909, &mut self.seq.track_mut(Device::Tr909.id() as usize));
        SongDevice::init_track(
            &mut self.devices.mixer,
            &mut self.seq.track_mut(Device::Mixer.id() as usize),
        );
    }

    fn install_song(
        &mut self,
        file: &rbs::Song,
        tracks: &[Vec<orbsong::TrackEvent>; 9],
        g: &orbsong::Globals,
        info: &SongInfo,
    ) {
        self.kept_song = Some(file.clone());
        self.seq = Sequencer::default();
        self.bound_at = 0;
        self.ensure_bound();
        self.init_device_tracks();
        for (i, evs) in tracks.iter().enumerate() {
            if !evs.is_empty() {
                let _ = self.seq.add_events(i, evs);
            }
        }
        self.seq.globals.loop_on = g.loop_on;
        self.seq.globals.tempo = if g.tempo == 0 {
            DEFAULT_TEMPO_MILLI
        } else {
            g.tempo
        };
        self.seq.globals.loop_start = g.loop_start;
        self.seq.globals.loop_end = g.loop_end.max(g.loop_start + orbsong::TICKS_PER_BAR);
        self.seq.globals.shuffle = g.shuffle;
        self.devices.mixer.live.channels[2].level = info.mix808;
        self.devices.mixer.live.channels[3].level = info.mix909;
        self.last_params.p808 = info.p808;
        self.last_params.p909 = info.p909;
        self.last_params.mix808 = info.mix808;
        self.last_params.mix909 = info.mix909;
        self.last_params.shuffle = info.shuffle;
        let mode = g.song_mode;
        self.with_devices(|seq, dev| {
            seq.set_song_mode(mode, dev);
            seq.locate(0, dev);
        });
        self.apply_params_live(&info.p808, &info.p909);
    }

    fn apply_params_live(&mut self, p808: &[u8; P808], p909: &[u8; P909]) {
        if self.song_mode() {
            return;
        }
        for (i, &v) in p808.iter().enumerate() {
            self.tr808.set_param(i as u8, v);
        }
        for (i, &v) in p909.iter().enumerate() {
            self.tr909.set_param(i as u8, v);
        }
    }

    pub fn reset_song(&mut self) {
        self.kept_song = None;
        self.seq = Sequencer::default();
        self.bound_at = 0;
        self.ensure_bound();
        self.init_device_tracks();
        self.last_params = Params::default();
        self.with_devices(|seq, dev| {
            seq.set_song_mode(0, dev);
            seq.locate(0, dev);
        });
    }

    pub fn transport(&self) -> Transport {
        let g = &self.seq.globals;
        let tick = self.seq.tick_pos();
        let start = g.loop_start as i32;
        let end = g.loop_end as i32;
        Transport {
            song_mode: i32::from(g.song_mode != 0),
            playing: i32::from(g.play != 0),
            recording: i32::from(g.byte2 != 0),
            loop_on: i32::from(g.loop_on != 0),
            bar: tick / BAR + 1,
            start: start / BAR + 1,
            length: ((end - start) / BAR).max(1),
            tempo_bpm: (g.tempo / 1000) as i32,
            tick,
        }
    }

    pub fn set_song_mode(&mut self, on: bool) {
        let mode = u8::from(on);
        if self.seq.globals.song_mode == mode {
            if !on {
                self.restart_clock();
            }
            return;
        }
        if !on {
            self.with_devices(|seq, dev| {
                seq.set_record(0, dev);
                seq.set_song_mode(0, dev);
            });
            self.restart_clock();
        } else {
            self.with_devices(|seq, dev| {
                seq.set_song_mode(1, dev);
            });
            // Next host callback Follows, so a DAW that was already playing locates and starts.
            self.restart_clock();
        }
    }

    pub fn play(&mut self) {
        self.with_devices(|seq, dev| seq.play(dev));
    }

    pub fn stop(&mut self) {
        self.with_devices(|seq, dev| {
            seq.set_record(0, dev);
            let song = seq.globals.song_mode != 0;
            if seq.globals.play != 0 {
                if song {
                    seq.stop(dev);
                } else {
                    seq.halt(dev);
                }
            } else if song {
                let pos = seq.tick_pos();
                let start = seq.globals.loop_start as i32;
                if start >= pos {
                    seq.locate(0, dev);
                } else {
                    seq.locate(start, dev);
                }
            }
        });
    }

    pub fn wind(&mut self, forward: bool) {
        self.with_devices(|seq, dev| {
            seq.set_record(0, dev);
            let pos = seq.tick_pos();
            let t = (if forward { 10 * BAR } else { -10 * BAR } + BAR * (pos / BAR)).clamp(0, MAX_POS);
            seq.locate(t, dev);
        });
    }

    pub fn bar_step(&mut self, up: bool) {
        if !self.song_mode() {
            return;
        }
        self.with_devices(|seq, dev| {
            if !up {
                seq.set_record(0, dev);
            }
            let pos = seq.tick_pos();
            let t = (if up { BAR } else { -BAR } + BAR * (pos / BAR)).clamp(0, MAX_POS);
            seq.locate(t, dev);
        });
    }

    pub fn loop_start_step(&mut self, delta_bars: i32) {
        if !self.song_mode() {
            return;
        }
        let len = self.seq.globals.loop_end as i32 - self.seq.globals.loop_start as i32;
        let start = BAR * ((self.seq.globals.loop_start as i32 + delta_bars * BAR).clamp(0, MAX_POS) / BAR);
        let end = (len + start).clamp(start + BAR, MAX_POS);
        self.seq.set_loop_start(start as u32);
        self.seq.set_loop_end(end as u32);
    }

    pub fn loop_length_step(&mut self, delta_bars: i32) {
        if !self.song_mode() {
            return;
        }
        let start = self.seq.globals.loop_start as i32;
        let hi = (MAX_END - start).max(BAR);
        let len = BAR * ((self.seq.globals.loop_end as i32 - start + delta_bars * BAR).clamp(BAR, hi) / BAR);
        self.seq.set_loop_end((start + len) as u32);
    }

    pub fn set_loop_on(&mut self, on: bool) {
        self.seq.set_loop_on(u8::from(on));
    }

    pub fn set_record(&mut self, on: bool) {
        if !self.song_mode() && on {
            return;
        }
        self.with_devices(|seq, dev| seq.set_record(u8::from(on), dev));
    }

    /// Replace song tracks with the pattern-mode snapshot. `loop_only` limits that to
    /// `[loop_start, loop_end)`.
    pub fn init_from_pattern(&mut self, loop_only: bool) {
        self.ensure_bound();
        let start = if loop_only {
            self.seq.globals.loop_start as i32
        } else {
            0
        };
        let end = self.seq.globals.loop_end as i32;
        let keep = if loop_only {
            self.events_outside(start, end)
        } else {
            std::array::from_fn(|_| Vec::new())
        };
        let g = self.seq.globals.clone();
        let pos = self.seq.tick_pos();
        let saved = self.seq.saved_tick;
        let last_tick = self.seq.last_tick;
        let playing = g.play != 0;

        self.seq = Sequencer::default();
        self.seq.globals = g;
        self.seq.pos = pos as f64;
        self.seq.last_tick = last_tick;
        self.seq.saved_tick = saved;
        self.bound_at = 0;
        self.ensure_bound();

        if loop_only {
            for (i, evs) in keep.iter().enumerate() {
                if !evs.is_empty() {
                    let _ = self.seq.add_events(i, evs);
                }
            }
            self.insert_pattern_at(start);
        } else {
            self.with_devices(|seq, dev| {
                for d in Device::ALL {
                    dev.device_mut(d)
                        .init_track(&mut seq.track_mut(d.id() as usize));
                }
            });
        }

        self.with_devices(|seq, dev| {
            seq.locate(pos, dev);
            if playing {
                seq.play(dev);
            }
        });
        self.sync_last_params_from_live();
    }

    fn events_outside(&self, start: i32, end: i32) -> [Vec<orbsong::TrackEvent>; 9] {
        std::array::from_fn(|i| {
            self.seq.tracks[i]
                .events(&self.seq.pool)
                .iter()
                .filter(|e| e.tick < start || e.tick >= end)
                .map(|e| orbsong::TrackEvent {
                    tick: e.tick.max(0) as u32,
                    ctrl: e.ctrl,
                    value: e.value,
                })
                .collect()
        })
    }

    fn insert_pattern_at(&mut self, tick: i32) {
        let p808 = self.tr808.params;
        let p909 = self.tr909.params_rbs_order();
        let mix = self.devices.mixer.pattern;
        {
            let mut tr = self.seq.track_mut(Device::Tr808.id() as usize);
            tr.set_quant(tr808::ENABLED as u8, 32);
            tr.set_quant(tr808::PATTERN as u8, 32);
            for c in 0..=tr808::LAST_CTRL {
                tr.insert(tick, c as u8, p808[c]);
            }
        }
        {
            let mut tr = self.seq.track_mut(Device::Tr909.id() as usize);
            tr.set_quant(tr909::ENABLED as u8, 32);
            tr.set_quant(tr909::PATTERN as u8, 32);
            for (c, &v) in p909.iter().enumerate().take(tr909::LAST_CTRL + 1) {
                tr.insert(tick, c as u8, v);
            }
        }
        {
            let mut tr = self.seq.track_mut(Device::Mixer.id() as usize);
            tr.insert(tick, 1, mix.comp_id);
            tr.insert(tick, 2, mix.pcf_id);
            for (c, ch) in mix.channels.iter().enumerate() {
                let base = 5 + 6 * c as u8;
                tr.insert(tick, base + 1, ch.level);
                tr.insert(tick, base + 2, ch.pan);
                tr.insert(tick, base + 3, ch.send);
                tr.insert(tick, base + 4, ch.dist);
            }
        }
    }

    fn sync_last_params_from_live(&mut self) {
        self.last_params.p808.copy_from_slice(&self.tr808.live[..P808]);
        self.last_params.p909 = self.rbs_909_live();
        self.last_params.mix808 = self.devices.mixer.live.channels[2].level;
        self.last_params.mix909 = self.devices.mixer.live.channels[3].level;
        self.last_params.shuffle = self.seq.globals.shuffle;
    }

    pub fn set_tempo_bpm(&mut self, bpm: i32) {
        let milli = (bpm.clamp(20, 500) as u32) * 1000;
        self.seq.set_tempo(milli);
    }

    pub fn tempo_milli(&self) -> u32 {
        if self.seq.globals.tempo == 0 {
            DEFAULT_TEMPO_MILLI
        } else {
            self.seq.globals.tempo
        }
    }

    pub fn record_param(&mut self, machine: i32, ctrl: u8, value: u8) {
        let (dev, max) = match machine {
            0 => (Device::Tr808, P808 as u8 - 1),
            1 => (Device::Tr909, P909 as u8 - 1),
            2 => (Device::Mixer, 27),
            _ => return,
        };
        if ctrl > max {
            return;
        }
        match machine {
            0 => self.tr808.set_param(ctrl, value),
            1 => self.tr909.set_param(ctrl, value),
            2 => self.devices.mixer.set_param(ctrl, value),
            _ => {}
        }
        self.seq.record_hook(dev, ctrl, value);
        match (machine, ctrl) {
            (0, c) if (c as usize) < P808 => self.last_params.p808[c as usize] = value,
            (1, c) if (c as usize) < P909 => self.last_params.p909[c as usize] = value,
            (2, MIX_808_LEVEL) => self.last_params.mix808 = value,
            (2, MIX_909_LEVEL) => self.last_params.mix909 = value,
            _ => {}
        }
    }

    fn apply_kit(&mut self, set: Arc<SampleSet>) {
        let mut retired = Retired::default();
        self.tr808.set_samples(&set, &mut retired);
        self.tr909.set_samples(&set, &mut retired);
        self.retired = Some(retired);
    }

    fn machine_patterns(&self, machine: i32) -> Option<&[u8]> {
        match machine {
            0 => Some(&self.tr808.patterns[..]),
            1 => Some(&self.tr909.patterns[..]),
            _ => None,
        }
    }

    fn machine_patterns_mut(&mut self, machine: i32) -> Option<&mut [u8]> {
        match machine {
            0 => Some(&mut self.tr808.patterns[..]),
            1 => Some(&mut self.tr909.patterns[..]),
            _ => None,
        }
    }

    pub fn get_patterns(&self, machine: i32, buf: &mut [u8]) -> i32 {
        let Some(src) = self.machine_patterns(machine) else {
            return 0;
        };
        let n = src.len().min(buf.len());
        buf[..n].copy_from_slice(&src[..n]);
        n as i32
    }

    pub fn set_patterns(&mut self, machine: i32, buf: &[u8]) -> i32 {
        let Some(dst) = self.machine_patterns_mut(machine) else {
            return 0;
        };
        let n = dst.len().min(buf.len());
        dst[..n].copy_from_slice(&buf[..n]);
        n as i32
    }

    fn step_at(pattern: u8, step: u8, col: u8) -> Option<usize> {
        if (pattern as usize) >= tr808::PATTERNS || (step as usize) >= 16 || (col as usize) >= 12 {
            return None;
        }
        Some(pattern as usize * tr808::PATTERN_LEN + 2 + 12 * step as usize + col as usize)
    }

    pub fn get_step(&self, machine: i32, pattern: u8, step: u8, col: u8) -> u8 {
        let Some(p) = self.machine_patterns(machine) else {
            return 0;
        };
        Self::step_at(pattern, step, col)
            .and_then(|i| p.get(i).copied())
            .unwrap_or(0)
    }

    pub fn set_step(&mut self, machine: i32, pattern: u8, step: u8, col: u8, value: u8) {
        let Some(p) = self.machine_patterns_mut(machine) else {
            return;
        };
        let Some(i) = Self::step_at(pattern, step, col) else {
            return;
        };
        p[i] = value;
        if machine == 1 && (col == tr909::CH as u8 || col == tr909::OH as u8) && value != 0 {
            let other = if col == tr909::CH as u8 {
                tr909::OH
            } else {
                tr909::CH
            };
            if let Some(j) = Self::step_at(pattern, step, other as u8) {
                p[j] = 0;
            }
        }
    }

    pub fn get_pat_shuffle(&self, machine: i32, pattern: u8) -> u8 {
        self.machine_patterns(machine)
            .and_then(|p| p.get(pattern as usize * tr808::PATTERN_LEN).copied())
            .unwrap_or(0)
    }

    pub fn set_pat_shuffle(&mut self, machine: i32, pattern: u8, on: u8) {
        if (pattern as usize) >= tr808::PATTERNS {
            return;
        }
        if let Some(p) = self.machine_patterns_mut(machine) {
            p[pattern as usize * tr808::PATTERN_LEN] = u8::from(on != 0);
        }
    }

    pub fn get_pat_length(&self, machine: i32, pattern: u8) -> u8 {
        self.machine_patterns(machine)
            .and_then(|p| p.get(pattern as usize * tr808::PATTERN_LEN + 1).copied())
            .unwrap_or(16)
    }

    pub fn set_pat_length(&mut self, machine: i32, pattern: u8, length: u8) {
        if (pattern as usize) >= tr808::PATTERNS {
            return;
        }
        if let Some(p) = self.machine_patterns_mut(machine) {
            p[pattern as usize * tr808::PATTERN_LEN + 1] = length.clamp(1, 16);
        }
    }

    /// Queue a MIDI hit. Fired on the next engine block after `apply_params`, so Enable does
    /// not gate it. `col` is a pattern column 1..=11.
    pub fn midi_trigger(&mut self, machine: i32, col: u8, velocity: u8) {
        if velocity == 0 || !(1..=11).contains(&col) || (machine != 0 && machine != 1) {
            return;
        }
        if self.pending_midi_n >= self.pending_midi.len() {
            return;
        }
        self.pending_midi[self.pending_midi_n] = (machine, col, velocity);
        self.pending_midi_n += 1;
    }

    fn drain_midi(&mut self) {
        let n = self.pending_midi_n;
        self.pending_midi_n = 0;
        for i in 0..n {
            let (machine, col, vel) = self.pending_midi[i];
            match machine {
                0 => self.tr808.midi_hit(col as usize, vel),
                1 => self.tr909.midi_hit(col as usize, vel),
                _ => {}
            }
        }
    }

    pub fn last_step(&self, machine: i32) -> i32 {
        let running = if self.song_mode() {
            self.seq.globals.play != 0
        } else {
            self.was_playing
        };
        if !running {
            return -1;
        }
        match machine {
            0 => self.tr808.live[29] as i32,
            1 => self.tr909.live[30] as i32,
            _ => -1,
        }
    }

    pub fn scan_dir(&mut self) {
        self.mods.clear();
        self.mods.push(ModEntry {
            path: PathBuf::new(),
            name: STANDARD_NAME.to_string(),
        });
        let dir = &self.mods_dir;
        if let Ok(rd) = std::fs::read_dir(dir) {
            let mut files: Vec<PathBuf> = rd
                .filter_map(Result::ok)
                .map(|e| e.path())
                .filter(|p| {
                    p.is_file() && p.extension().is_some_and(|x| x.eq_ignore_ascii_case("rbm"))
                })
                .collect();
            files.sort_by(|a, b| byte_order(a, b));
            for p in files {
                let name = std::fs::read(&p)
                    .ok()
                    .and_then(|b| Rbm::parse(b).ok())
                    .and_then(|r| r.info().map(|i| i.name.clone()))
                    .unwrap_or_else(|| {
                        p.file_stem()
                            .map(|s| s.to_string_lossy().into_owned())
                            .unwrap_or_default()
                    });
                self.mods.push(ModEntry { path: p, name });
            }
        }
        if self.current >= self.mods.len() {
            self.current = 0;
        }
    }

    pub fn scan(&mut self, folder: &Path) {
        self.mods_dir = folder.to_path_buf();
        self.scan_dir();
    }

    pub fn mod_count(&self) -> i32 {
        self.mods.len() as i32
    }

    pub fn current_mod(&self) -> i32 {
        self.current as i32
    }

    pub fn mod_name(&self, index: i32) -> &str {
        self.mods
            .get(index as usize)
            .map(|m| m.name.as_str())
            .unwrap_or("")
    }

    pub fn mod_path(&self, index: i32) -> String {
        self.mods
            .get(index as usize)
            .map(|m| m.path.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    pub fn mods_folder(&self) -> &str {
        self.mods_dir.to_str().unwrap_or("")
    }

    pub fn status(&self) -> &str {
        &self.status
    }

    pub fn dat_path(&self) -> &str {
        self.dat_path
            .as_ref()
            .and_then(|p| p.to_str())
            .unwrap_or("")
    }

    /// JPEG bytes of the sounding kit's `splash.jpg` (empty if the kit has none).
    pub fn splash_jpeg(&self) -> &[u8] {
        &self.splash
    }

    pub fn set_dat_path(&mut self, path: &Path) {
        self.dat_path = Some(path.to_path_buf());
    }

    /// Load kit `index`. Returns 1 on success. A failed load keeps the previous kit and sets
    /// [`Self::status`].
    pub fn load(&mut self, index: i32) -> i32 {
        let i = index as usize;
        if i >= self.mods.len() {
            return 0;
        }
        if i == 0 {
            self.apply_kit(standard::builtin_samples());
            self.splash = builtin_splash();
            self.current = 0;
            self.status.clear();
            return 1;
        }
        let path = self.mods[i].path.clone();
        let dat = self.dat_path.clone();
        let mods_dir = self.mods_dir.clone();
        match load_kit(&path, dat.as_deref(), &mods_dir) {
            Ok(kit) => {
                self.apply_kit(kit.set);
                self.splash = kit.splash;
                self.current = i;
                self.status.clear();
                1
            }
            Err(msg) => {
                // Keep the list selection on the failed kit so Settings can retry it.
                // Samples stay on the previous kit.
                self.current = i;
                self.status = msg;
                0
            }
        }
    }
}

fn with_mut<T>(m: *mut Modloader, f: impl FnOnce(&mut Modloader) -> T, fallback: T) -> T {
    match unsafe { m.as_mut() } {
        Some(p) => f(p),
        None => fallback,
    }
}

fn with_ref<T>(m: *mut Modloader, f: impl FnOnce(&Modloader) -> T, fallback: T) -> T {
    match unsafe { m.as_ref() } {
        Some(p) => f(p),
        None => fallback,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_modloader_new() -> *mut Modloader {
    Box::into_raw(Box::new(Modloader::new()))
}

/// # Safety
/// `m` is a pointer from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_free(m: *mut Modloader) {
    if !m.is_null() {
        drop(unsafe { Box::from_raw(m) });
    }
}

/// # Safety
/// `m` is a pointer from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_reset(m: *mut Modloader) {
    with_mut(m, |p| p.reset(), ());
}

/// # Safety
/// `m` is a pointer from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_restart_clock(m: *mut Modloader) {
    with_mut(m, |p| p.restart_clock(), ());
}

#[unsafe(no_mangle)]
pub extern "C" fn orb_modloader_latency() -> i32 {
    Modloader::latency() as i32
}

/// # Safety
/// `m` is a pointer from [`orb_modloader_new`]. `main_l` and `main_r` each have `nframes`
/// samples when `nframes > 0`. `mix808` and `mix909` are each `nframes`, or null to skip
/// that machine mix. `aux` is 22 pointers, each `nframes`, or null to skip aux.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_process(
    m: *mut Modloader,
    main_l: *mut f32,
    main_r: *mut f32,
    mix808: *mut f32,
    mix909: *mut f32,
    aux: *const *mut f32,
    nframes: i32,
    params: *const Params,
    playhead: *const PlayheadC,
) {
    let Some(p) = (unsafe { m.as_mut() }) else {
        return;
    };
    if nframes <= 0 || main_l.is_null() || main_r.is_null() {
        return;
    }
    let n = nframes as usize;
    let main_l = unsafe { std::slice::from_raw_parts_mut(main_l, n) };
    let main_r = unsafe { std::slice::from_raw_parts_mut(main_r, n) };
    let mut skip808: [f32; 0] = [];
    let mut skip909: [f32; 0] = [];
    let mix808 = if mix808.is_null() {
        skip808.as_mut_slice()
    } else {
        unsafe { std::slice::from_raw_parts_mut(mix808, n) }
    };
    let mix909 = if mix909.is_null() {
        skip909.as_mut_slice()
    } else {
        unsafe { std::slice::from_raw_parts_mut(mix909, n) }
    };
    let params = unsafe { params.as_ref() }.copied().unwrap_or_default();
    let playhead = unsafe { playhead.as_ref() }
        .copied()
        .map(Playhead::from)
        .unwrap_or_default();

    if aux.is_null() {
        let mut empty: [&mut [f32]; 0] = [];
        p.process(main_l, main_r, mix808, mix909, &mut empty, params, playhead);
        return;
    }
    let ptrs = unsafe { std::slice::from_raw_parts(aux, AUX) };
    let mut aux_vec: Vec<&mut [f32]> = Vec::with_capacity(AUX);
    for &ptr in ptrs {
        if ptr.is_null() {
            let mut empty: [&mut [f32]; 0] = [];
            p.process(main_l, main_r, mix808, mix909, &mut empty, params, playhead);
            return;
        }
        aux_vec.push(unsafe { std::slice::from_raw_parts_mut(ptr, n) });
    }
    p.process(main_l, main_r, mix808, mix909, &mut aux_vec, params, playhead);
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_get_patterns(
    m: *mut Modloader,
    machine: i32,
    buf: *mut u8,
    len: i32,
) -> i32 {
    if buf.is_null() || len <= 0 {
        return 0;
    }
    let buf = unsafe { std::slice::from_raw_parts_mut(buf, len as usize) };
    with_ref(m, |p| p.get_patterns(machine, buf), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_patterns(
    m: *mut Modloader,
    machine: i32,
    buf: *const u8,
    len: i32,
) -> i32 {
    if buf.is_null() || len <= 0 {
        return 0;
    }
    let buf = unsafe { std::slice::from_raw_parts(buf, len as usize) };
    with_mut(m, |p| p.set_patterns(machine, buf), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_get_step(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
    step: u8,
    col: u8,
) -> u8 {
    with_ref(m, |p| p.get_step(machine, pattern, step, col), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_step(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
    step: u8,
    col: u8,
    value: u8,
) {
    with_mut(m, |p| p.set_step(machine, pattern, step, col, value), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_get_pat_shuffle(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
) -> u8 {
    with_ref(m, |p| p.get_pat_shuffle(machine, pattern), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_pat_shuffle(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
    on: u8,
) {
    with_mut(m, |p| p.set_pat_shuffle(machine, pattern, on), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_get_pat_length(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
) -> u8 {
    with_ref(m, |p| p.get_pat_length(machine, pattern), 16)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_pat_length(
    m: *mut Modloader,
    machine: i32,
    pattern: u8,
    length: u8,
) {
    with_mut(m, |p| p.set_pat_length(machine, pattern, length), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_last_step(m: *mut Modloader, machine: i32) -> i32 {
    with_ref(m, |p| p.last_step(machine), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null. `col` is a pattern column 1..=11.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_trigger(
    m: *mut Modloader,
    machine: i32,
    col: u8,
    velocity: u8,
) {
    with_mut(m, |p| p.midi_trigger(machine, col, velocity), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `folder` is a NUL-terminated path, or null (rescan).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_scan(m: *mut Modloader, folder: *const i8) {
    with_mut(
        m,
        |p| {
            if folder.is_null() {
                p.scan_dir();
            } else {
                let s = unsafe { std::ffi::CStr::from_ptr(folder) }
                    .to_string_lossy()
                    .into_owned();
                p.scan(Path::new(&s));
            }
        },
        (),
    );
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_mod_count(m: *mut Modloader) -> i32 {
    with_ref(m, |p| p.mod_count(), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_current_mod(m: *mut Modloader) -> i32 {
    with_ref(m, |p| p.current_mod(), 0)
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `buflen` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_mod_name(
    m: *mut Modloader,
    index: i32,
    buf: *mut i8,
    buflen: i32,
) {
    with_ref(m, |p| copy_cstr(p.mod_name(index), buf, buflen), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `buflen` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_mod_path(
    m: *mut Modloader,
    index: i32,
    buf: *mut i8,
    buflen: i32,
) {
    with_ref(m, |p| copy_cstr(&p.mod_path(index), buf, buflen), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `buflen` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_mods_folder(m: *mut Modloader, buf: *mut i8, buflen: i32) {
    with_ref(m, |p| copy_cstr(p.mods_folder(), buf, buflen), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `buflen` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_status(m: *mut Modloader, buf: *mut i8, buflen: i32) {
    with_ref(m, |p| copy_cstr(p.status(), buf, buflen), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `buf` has `buflen` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_dat_path(m: *mut Modloader, buf: *mut i8, buflen: i32) {
    with_ref(m, |p| copy_cstr(p.dat_path(), buf, buflen), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `path` is a NUL-terminated path, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_dat_path(m: *mut Modloader, path: *const i8) {
    with_mut(
        m,
        |p| {
            if path.is_null() {
                p.dat_path = None;
            } else {
                let s = unsafe { std::ffi::CStr::from_ptr(path) }
                    .to_string_lossy()
                    .into_owned();
                p.set_dat_path(Path::new(&s));
            }
        },
        (),
    );
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_load(m: *mut Modloader, index: i32) -> i32 {
    with_mut(m, |p| p.load(index), 0)
}

/// Copies the sounding kit's `splash.jpg` into `buf`.
///
/// # Safety
/// `m` from [`orb_modloader_new`], or null. `buf` is null to query the size, or has `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_splash_jpeg(
    m: *mut Modloader,
    buf: *mut u8,
    len: i32,
) -> i32 {
    with_ref(
        m,
        |p| {
            let src = p.splash_jpeg();
            if buf.is_null() || len <= 0 {
                return src.len() as i32;
            }
            let n = src.len().min(len as usize);
            unsafe {
                std::ptr::copy_nonoverlapping(src.as_ptr(), buf, n);
            }
            n as i32
        },
        0,
    )
}

/// Parse a ReBirth 2.x `.rbs` and apply its 808/909 patterns. Returns 1 and fills `out` on
/// success. On failure returns 0 and sets [`orb_modloader_status`].
///
/// # Safety
/// `m` from [`orb_modloader_new`]. `data` has `len` bytes. `out` is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_load_song(
    m: *mut Modloader,
    data: *const u8,
    len: i32,
    out: *mut SongInfo,
) -> i32 {
    if data.is_null() || len <= 0 || out.is_null() {
        return 0;
    }
    let bytes = unsafe { std::slice::from_raw_parts(data, len as usize) };
    with_mut(
        m,
        |p| match p.load_song(bytes) {
            Ok(info) => {
                unsafe {
                    *out = info;
                }
                1
            }
            Err(msg) => {
                p.status = msg;
                0
            }
        },
        0,
    )
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_reset_song(m: *mut Modloader) {
    with_mut(m, |p| p.reset_song(), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `out` is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_transport(m: *mut Modloader, out: *mut Transport) {
    if out.is_null() {
        return;
    }
    unsafe {
        *out = with_ref(m, |p| p.transport(), Transport::default());
    }
}

/// # Safety
/// `m` from [`orb_modloader_new`]. `out` is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_live_params(m: *mut Modloader, out: *mut Params) {
    if out.is_null() {
        return;
    }
    unsafe {
        *out = with_ref(m, |p| p.live_params(), Params::default());
    }
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_song_mode(m: *mut Modloader, on: i32) {
    with_mut(m, |p| p.set_song_mode(on != 0), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_play(m: *mut Modloader) {
    with_mut(m, |p| p.play(), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_stop(m: *mut Modloader) {
    with_mut(m, |p| p.stop(), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_wind(m: *mut Modloader, forward: i32) {
    with_mut(m, |p| p.wind(forward != 0), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_bar_step(m: *mut Modloader, up: i32) {
    with_mut(m, |p| p.bar_step(up != 0), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_loop_start_step(m: *mut Modloader, delta_bars: i32) {
    with_mut(m, |p| p.loop_start_step(delta_bars), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_loop_length_step(m: *mut Modloader, delta_bars: i32) {
    with_mut(m, |p| p.loop_length_step(delta_bars), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_loop_on(m: *mut Modloader, on: i32) {
    with_mut(m, |p| p.set_loop_on(on != 0), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_record(m: *mut Modloader, on: i32) {
    with_mut(m, |p| p.set_record(on != 0), ());
}

/// Copy pattern-mode state onto the song. `loop_only` 1 is the current loop; 0 is the whole song.
///
/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_init_from_pattern(m: *mut Modloader, loop_only: i32) {
    with_mut(m, |p| p.init_from_pattern(loop_only != 0), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_set_tempo_bpm(m: *mut Modloader, bpm: i32) {
    with_mut(m, |p| p.set_tempo_bpm(bpm), ());
}

/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_tempo_milli(m: *mut Modloader) -> u32 {
    with_ref(m, |p| p.tempo_milli(), DEFAULT_TEMPO_MILLI)
}

/// Record a panel change in song mode. `machine` is 0 808, 1 909, 2 mixer.
///
/// # Safety
/// `m` from [`orb_modloader_new`], or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_record_param(
    m: *mut Modloader,
    machine: i32,
    ctrl: u8,
    value: u8,
) {
    with_mut(m, |p| p.record_param(machine, ctrl, value), ());
}

/// Write a ReBirth 2.x `.rbs` from the current 808/909 patterns and `in`. If `buf` is null
/// or `len` is 0, returns the byte size. Returns 0 on error (see [`orb_modloader_status`]).
///
/// # Safety
/// `m` from [`orb_modloader_new`]. `in` is non-null. `buf` is null or has `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_save_song(
    m: *mut Modloader,
    info: *const SongInfo,
    buf: *mut u8,
    len: i32,
) -> i32 {
    let Some(info) = (unsafe { info.as_ref() }) else {
        return 0;
    };
    with_mut(
        m,
        |p| match p.save_song(info) {
            Ok(bytes) => {
                if buf.is_null() || len <= 0 {
                    return bytes.len() as i32;
                }
                if (len as usize) < bytes.len() {
                    return bytes.len() as i32;
                }
                unsafe {
                    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf, bytes.len());
                }
                bytes.len() as i32
            }
            Err(msg) => {
                p.status = msg;
                0
            }
        },
        0,
    )
}

#[cfg(test)]
mod tests;
