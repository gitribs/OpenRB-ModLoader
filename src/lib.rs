//! TR-808 and TR-909 wrapper around [`orb_engine`], for the OpenRB Modloader plugin.
//!
//! Both machines share one [`ProcessState`] and one [`SampleSet`]. Audio is processed in
//! 64-sample blocks at 44100 Hz. Output is delayed by one block so a host sees a constant
//! 64-sample latency. The C API takes (and returns) buffers already at 44100 Hz; resampling
//! is the JUCE plugin's job.
//!
//! The clock is ReBirth's song position, the same Follow / FreeRun / Hold split as OpenRB PCF.
//! Pattern mode only: the selected pattern loops. Shuffle uses the global knob when that
//! pattern's shuffle flag is on.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Arc;

mod iso;

use orb_engine::frame::{BLOCK, Block, ChannelDevice, SongDevice};
use orb_engine::process::ProcessState;
use orb_engine::samples::{Retired, SampleSet};
use orb_engine::seq::{self, TEMPO_STEP};
use orb_engine::song::PPQ;
use orb_engine::{tr808, tr909};
use rbm::{KeyTable, Rbm, RbmError, standard};

/// Engine (and original) sample rate.
pub const ENGINE_RATE: f64 = 44100.0;
/// Fallback tempo when the host has none, as BPM*1000.
pub const DEFAULT_TEMPO_MILLI: u32 = 120_000;
/// Aux buses: 11 instruments per machine.
pub const AUX: usize = 22;
/// Controllers 0..=28 of the 808.
pub const P808: usize = 29;
/// Controllers 0..=29 of the 909 (RBS / panel order).
pub const P909: usize = 30;
/// One machine's pattern dump: 32 × 194.
pub const PATTERN_BYTES: usize = tr808::PATTERNS * tr808::PATTERN_LEN;
/// Default ReBirth Mods folder on macOS.
pub const DEFAULT_MODS_DIR: &str = "/Library/Application Support/Propellerhead/Rebirth RB-338/Mods";

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
        }
    }
}

/// Host playhead at the first 44100 Hz frame of this callback.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Playhead {
    pub bpm: f64,
    pub ppq: f64,
    pub playing: bool,
}

/// C layout of [`Playhead`]: `playing` is a 32-bit flag, not a Rust `bool`.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct PlayheadC {
    pub bpm: f64,
    pub ppq: f64,
    pub playing: i32,
}

impl From<PlayheadC> for Playhead {
    fn from(p: PlayheadC) -> Self {
        Self {
            bpm: p.bpm,
            ppq: p.ppq,
            playing: p.playing != 0,
        }
    }
}

impl Default for Playhead {
    fn default() -> Self {
        Self {
            bpm: 120.0,
            ppq: 0.0,
            playing: false,
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

fn load_kit(path: &Path, dat: Option<&Path>, mods_dir: &Path) -> Result<Arc<SampleSet>, String> {
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
        Ok(loaded) => Ok(Arc::new(loaded.set)),
        Err(RbmError::KeyNotCovered(_)) => Err("needs DAT/ISO".to_string()),
        Err(RbmError::WrongKey(_)) => Err("bad key".to_string()),
        Err(e) => Err(e.to_string()),
    }
}

/// Both drum machines, one clock, a 64-sample output delay, and the scanned mod list.
pub struct Modloader {
    tr808: tr808::Tr808,
    tr909: tr909::Tr909,
    retired: Option<Retired>,
    work_main: Block,
    work_aux: [Block; AUX],
    delay_main: Block,
    delay_aux: [Block; AUX],
    fill: usize,
    pos: f64,
    was_playing: bool,
    last_ppq: Option<f64>,
    held_tick: i32,
    last_tick: Option<i32>,
    blocks_processed: u64,
    mods: Vec<ModEntry>,
    mods_dir: PathBuf,
    dat_path: Option<PathBuf>,
    current: usize,
    status: String,
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
        let mut m = Self {
            tr808: tr808::Tr808::new(samples.clone(), process.clone()),
            tr909: tr909::Tr909::new(samples, process),
            retired: None,
            work_main: [0.0; BLOCK],
            work_aux: [[0.0; BLOCK]; AUX],
            delay_main: [0.0; BLOCK],
            delay_aux: [[0.0; BLOCK]; AUX],
            fill: 0,
            pos: 0.0,
            was_playing: false,
            last_ppq: None,
            held_tick: 0,
            last_tick: None,
            blocks_processed: 0,
            mods: Vec::new(),
            mods_dir,
            dat_path: None,
            current: 0,
            status: String::new(),
        };
        m.scan_dir();
        m
    }

    /// Clear the output delay and clock. Patterns, the loaded kit, and the DAT path stay;
    /// hosts call this on bus connect and on prepareToPlay.
    pub fn reset(&mut self) {
        self.work_main = [0.0; BLOCK];
        self.work_aux = [[0.0; BLOCK]; AUX];
        self.delay_main = [0.0; BLOCK];
        self.delay_aux = [[0.0; BLOCK]; AUX];
        self.fill = 0;
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
        aux: &mut [&mut [f32]],
        params: Params,
        playhead: Playhead,
    ) {
        let n = main_l.len().min(main_r.len());
        let tempo = tempo_milli(playhead.bpm);
        let mode = self.begin_clock(playhead, n);
        let mut i = 0;
        let mut k = 0usize;

        while i < n {
            let take = (n - i).min(BLOCK - self.fill);
            main_l[i..i + take].copy_from_slice(&self.delay_main[self.fill..self.fill + take]);
            main_r[i..i + take].copy_from_slice(&self.delay_main[self.fill..self.fill + take]);
            for (ch, bus) in aux.iter_mut().enumerate().take(AUX) {
                if i + take <= bus.len() {
                    bus[i..i + take]
                        .copy_from_slice(&self.delay_aux[ch][self.fill..self.fill + take]);
                }
            }
            self.fill += take;
            i += take;

            if self.fill == BLOCK {
                let tick = self.take_tick(playhead, k, mode);
                self.apply_params(params);
                let shuffled = seq::shuffled(tick, params.shuffle);
                self.tr808.step(tick, shuffled, tempo as i32);
                self.tr909.step(tick, shuffled, tempo as i32);

                let mut v808 = [[0.0f32; BLOCK]; tr808::VOICES];
                let mut v909 = [[0.0f32; BLOCK]; tr909::VOICES];
                let mut mix808 = [0.0f32; BLOCK];
                let mut mix909 = [0.0f32; BLOCK];
                let mut d808 = [false; tr808::VOICES];
                let mut d909 = [false; tr909::VOICES];
                for i in 0..tr808::VOICES {
                    d808[i] = params.direct[i] != 0;
                    d909[i] = params.direct[tr808::VOICES + i] != 0;
                }
                self.tr808.process_mix(&mut mix808, &mut v808, &d808);
                self.tr909.process_mix(&mut mix909, &mut v909, &d909);

                if params.bypass == 0 {
                    for s in 0..BLOCK {
                        self.work_main[s] = mix808[s] + mix909[s];
                    }
                    for (i, v) in v808.iter().enumerate() {
                        self.work_aux[i] = *v;
                    }
                    for (i, v) in v909.iter().enumerate() {
                        self.work_aux[tr808::VOICES + i] = *v;
                    }
                } else {
                    self.work_main = [0.0; BLOCK];
                    self.work_aux = [[0.0; BLOCK]; AUX];
                }

                if mode != ClockMode::Hold {
                    self.held_tick = tick;
                }
                self.last_tick = Some(tick);
                std::mem::swap(&mut self.work_main, &mut self.delay_main);
                std::mem::swap(&mut self.work_aux, &mut self.delay_aux);
                self.fill = 0;
                self.blocks_processed += 1;
                k += 1;
            }
        }
    }

    fn begin_clock(&mut self, playhead: Playhead, n: usize) -> ClockMode {
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
        if !self.was_playing {
            let tick = seq::trunc(host_pos(playhead.ppq, playhead.bpm, 0));
            self.tr808.start(tick);
            self.tr909.start(tick);
            self.tr808.relocate(tick, tick);
            self.tr909.relocate(tick, tick);
        }
        self.was_playing = true;
        self.last_ppq = Some(playhead.ppq);
        mode
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

    pub fn last_step(&self, machine: i32) -> i32 {
        if !self.was_playing {
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
            self.current = 0;
            self.status.clear();
            return 1;
        }
        let path = self.mods[i].path.clone();
        let dat = self.dat_path.clone();
        let mods_dir = self.mods_dir.clone();
        match load_kit(&path, dat.as_deref(), &mods_dir) {
            Ok(set) => {
                self.apply_kit(set);
                self.current = i;
                self.status.clear();
                1
            }
            Err(msg) => {
                // Keep the list selection on the failed kit so DAT… can retry it.
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
/// samples when `nframes > 0`. `aux` is 22 pointers, each `nframes`, or null to skip aux.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn orb_modloader_process(
    m: *mut Modloader,
    main_l: *mut f32,
    main_r: *mut f32,
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
    let params = unsafe { params.as_ref() }.copied().unwrap_or_default();
    let playhead = unsafe { playhead.as_ref() }
        .copied()
        .map(Playhead::from)
        .unwrap_or_default();

    if aux.is_null() {
        let mut empty: [&mut [f32]; 0] = [];
        p.process(main_l, main_r, &mut empty, params, playhead);
        return;
    }
    let ptrs = unsafe { std::slice::from_raw_parts(aux, AUX) };
    let mut aux_vec: Vec<&mut [f32]> = Vec::with_capacity(AUX);
    for &ptr in ptrs {
        if ptr.is_null() {
            let mut empty: [&mut [f32]; 0] = [];
            p.process(main_l, main_r, &mut empty, params, playhead);
            return;
        }
        aux_vec.push(unsafe { std::slice::from_raw_parts_mut(ptr, n) });
    }
    p.process(main_l, main_r, &mut aux_vec, params, playhead);
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

#[cfg(test)]
mod tests;
