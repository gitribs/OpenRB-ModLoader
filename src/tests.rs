use super::*;
use std::path::Path;

use orb_engine::{tr808, tr909};

fn jpeg_size(data: &[u8]) -> Option<(u16, u16)> {
    let mut i = 0;
    while i + 1 < data.len() {
        if data[i] != 0xFF {
            i += 1;
            continue;
        }
        let marker = data[i + 1];
        if marker == 0xD8 || marker == 0xD9 || (0xD0..=0xD7).contains(&marker) || marker == 0x01 {
            i += 2;
            continue;
        }
        if i + 3 >= data.len() {
            break;
        }
        let len = u16::from_be_bytes([data[i + 2], data[i + 3]]) as usize;
        if matches!(
            marker,
            0xC0 | 0xC1
                | 0xC2
                | 0xC3
                | 0xC5
                | 0xC6
                | 0xC7
                | 0xC9
                | 0xCA
                | 0xCB
                | 0xCD
                | 0xCE
                | 0xCF
        ) && i + 8 < data.len()
        {
            let h = u16::from_be_bytes([data[i + 5], data[i + 6]]);
            let w = u16::from_be_bytes([data[i + 7], data[i + 8]]);
            return Some((w, h));
        }
        if len < 2 {
            break;
        }
        i += 2 + len;
    }
    None
}

fn playing(ppq: f64) -> Playhead {
    Playhead {
        bpm: 120.0,
        ppq,
        playing: true,
    }
}

fn run_all(
    m: &mut Modloader,
    params: Params,
    playhead: Playhead,
    n: usize,
) -> (Vec<f32>, Vec<f32>, Vec<f32>, Vec<Vec<f32>>) {
    let mut left = vec![0.0f32; n];
    let mut right = vec![0.0f32; n];
    let mut mix808 = vec![0.0f32; n];
    let mut mix909 = vec![0.0f32; n];
    let mut aux_store = vec![vec![0.0f32; n]; AUX];
    let mut aux_refs: Vec<&mut [f32]> = aux_store.iter_mut().map(|v| v.as_mut_slice()).collect();
    m.process(
        &mut left,
        &mut right,
        &mut mix808,
        &mut mix909,
        &mut aux_refs,
        params,
        playhead,
    );
    (left, mix808, mix909, aux_store)
}

fn run(
    m: &mut Modloader,
    params: Params,
    playhead: Playhead,
    n: usize,
) -> (Vec<f32>, Vec<Vec<f32>>) {
    let (left, _, _, aux) = run_all(m, params, playhead, n);
    (left, aux)
}

fn energy(buf: &[f32]) -> f32 {
    buf.iter().map(|x| x.abs()).sum()
}

/// Built-in Standard ReBirth exposes splash.jpg as a JPEG (the bottom sprite of the
/// packed 258x1934 panel atlas used by mod tools).
#[test]
fn builtin_kit_has_splash_jpeg() {
    let mut m = Modloader::new();
    let jpeg = m.splash_jpeg().to_vec();
    assert!(jpeg.starts_with(&[0xFF, 0xD8]), "splash is a JPEG");
    assert_eq!(jpeg_size(&jpeg), Some((256, 256)));
    assert_eq!(m.load(0), 1);
    assert_eq!(m.splash_jpeg(), jpeg);
}

/// Built-in Standard ReBirth constructs both machines and is always list item 0.
#[test]
fn builtin_kit_constructs() {
    let m = Modloader::new();
    assert_eq!(m.mod_name(0), STANDARD_NAME);
    assert_eq!(m.current_mod(), 0);
    assert!(m.mod_count() >= 1);
    assert_eq!(m.get_pat_length(0, 0), 16);
    assert_eq!(m.get_pat_length(1, 0), 16);
    assert_eq!(m.get_step(0, 0, 0, tr808::BD as u8), 0);
}

/// A BD step on pattern 0 produces audio after the 64-sample output delay.
#[test]
fn step_trigger_makes_sound() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut params = Params::default();
    params.p808[tr808::ENABLED] = 1;
    params.p808[tr808::PATTERN] = 0;
    params.p808[tr808::BD_LEVEL] = 127;
    params.p808[tr808::ACCENT] = 127;

    let n = BLOCK * 4;
    let (left, aux) = run(&mut m, params, playing(0.0), n);
    assert!(
        left[..BLOCK].iter().all(|&x| x == 0.0),
        "first block is the delay"
    );
    assert!(
        energy(&aux[tr808::VOICE_BD][BLOCK..]) > 0.0,
        "808 BD aux should ring after the delay"
    );
    assert!(energy(&left[BLOCK..]) > 0.0, "main mix includes BD");
}

/// `set_samples` (reload Standard) keeps pattern bytes.
#[test]
fn mod_swap_keeps_pattern() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 3, tr808::SD as u8, 1);
    m.set_step(1, 0, 5, tr909::LT as u8, 2);
    m.set_pat_shuffle(0, 0, 1);
    m.set_pat_length(1, 0, 12);
    assert_eq!(m.load(0), 1);
    assert_eq!(m.get_step(0, 0, 3, tr808::SD as u8), 1);
    assert_eq!(m.get_step(1, 0, 5, tr909::LT as u8), 2);
    assert_eq!(m.get_pat_shuffle(0, 0), 1);
    assert_eq!(m.get_pat_length(1, 0), 12);
}

/// Pattern shuffle + global amount delays the offbeat 16th. Amounts 0–5 are identity.
/// Shuffle is tick-based; it does not depend on the host BPM parameter.
#[test]
fn pattern_shuffle_delays_offbeat() {
    let first_hit = |flag: u8, amount: u8| -> usize {
        let mut m = Modloader::new();
        m.set_step(0, 0, 1, tr808::BD as u8, 1);
        m.set_pat_shuffle(0, 0, flag);
        let mut params = Params::default();
        params.p808[tr808::ENABLED] = 1;
        params.p808[tr808::BD_LEVEL] = 127;
        params.p808[tr808::ACCENT] = 127;
        params.shuffle = amount;
        let n = BLOCK * 400;
        let (_, aux) = run(&mut m, params, playing(0.0), n);
        aux[tr808::VOICE_BD]
            .iter()
            .skip(BLOCK)
            .position(|&x| x.abs() > 1.0e-5)
            .expect("BD on step 1 should sound")
    };
    let straight = first_hit(0, 127);
    let amount_off = first_hit(1, 0);
    let swung = first_hit(1, 127);
    assert_eq!(
        straight, amount_off,
        "amount 0 is identity even with the flag on"
    );
    assert!(
        swung > straight + BLOCK,
        "flag + amount 127 should delay the offbeat (straight {straight}, swung {swung})"
    );
}

/// Mixer channel level is the original 808/909 volume fader: it scales the stereo mix
/// and that machine's Mix bus with ReBirth's level curve, and leaves the aux buses alone.
#[test]
fn mixer_volume_scales_main_mix() {
    let n = BLOCK * 8;
    let run_mix = |mix: u8| {
        let mut m = Modloader::new();
        m.set_step(0, 0, 0, tr808::BD as u8, 1);
        let mut params = Params::default();
        params.p808[tr808::ENABLED] = 1;
        params.p808[tr808::BD_LEVEL] = 127;
        params.p808[tr808::ACCENT] = 127;
        params.mix808 = mix;
        params.mix909 = 0;
        run(&mut m, params, playing(0.0), n)
    };
    let (full, aux_full) = run_mix(127);
    let (def, _) = run_mix(100);
    let (off, aux_off) = run_mix(0);
    let e_full = energy(&full[BLOCK..]);
    let e_def = energy(&def[BLOCK..]);
    let e_off = energy(&off[BLOCK..]);
    assert!(e_full > 0.0, "max mixer level still sounds");
    assert!(
        e_full > e_def,
        "127 is louder than the ReBirth default of 100"
    );
    assert!(e_off < e_full * 1.0e-3, "mixer 0 silences the main mix");
    assert!(
        energy(&aux_full[tr808::VOICE_BD][BLOCK..]) > 0.0
            && energy(&aux_off[tr808::VOICE_BD][BLOCK..]) > 0.0,
        "aux buses are independent of the mixer fader"
    );
}

/// Each machine Mix bus is the mixer-scaled sum of that machine, so main = 808 Mix + 909 Mix.
#[test]
fn machine_mix_buses_split_the_sum() {
    let n = BLOCK * 8;
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    m.set_step(1, 0, 0, tr909::BD as u8, 1);
    let mut params = Params::default();
    params.p808[tr808::ENABLED] = 1;
    params.p808[tr808::BD_LEVEL] = 127;
    params.p808[tr808::ACCENT] = 127;
    params.p909[tr909::ENABLED] = 1;
    params.p909[tr909::BD_LEVEL] = 127;
    params.p909[tr909::ACCENT] = 127;
    params.mix808 = 100;
    params.mix909 = 100;
    let (left, mix808, mix909, _) = run_all(&mut m, params, playing(0.0), n);
    assert!(energy(&mix808[BLOCK..]) > 0.0, "808 Mix has the 808");
    assert!(energy(&mix909[BLOCK..]) > 0.0, "909 Mix has the 909");
    for i in BLOCK..n {
        let sum = mix808[i] + mix909[i];
        assert!(
            (left[i] - sum).abs() < 1.0e-5,
            "main is 808 Mix + 909 Mix at {i}"
        );
    }

    let mut only808 = Modloader::new();
    only808.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut p808 = Params::default();
    p808.p808[tr808::ENABLED] = 1;
    p808.p808[tr808::BD_LEVEL] = 127;
    p808.p808[tr808::ACCENT] = 127;
    p808.mix909 = 0;
    let (_, mix808_only, mix909_only, _) = run_all(&mut only808, p808, playing(0.0), n);
    assert!(energy(&mix808_only[BLOCK..]) > 0.0);
    assert!(
        energy(&mix909_only[BLOCK..]) == 0.0,
        "808 does not leak onto 909 Mix"
    );

    let mut muted = Modloader::new();
    muted.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut p_mute = p808;
    p_mute.mix808 = 0;
    let (_, mix_off, _, aux_off) = run_all(&mut muted, p_mute, playing(0.0), n);
    assert!(
        energy(&mix_off[BLOCK..]) < energy(&mix808_only[BLOCK..]) * 1.0e-3,
        "mixer 0 silences 808 Mix"
    );
    assert!(
        energy(&aux_off[tr808::VOICE_BD][BLOCK..]) > 0.0,
        "instrument aux is independent of the mixer fader"
    );

    let mut routed = Modloader::new();
    routed.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut p_dir = p808;
    p_dir.direct[tr808::VOICE_BD] = 1;
    let (_, mix_dir, _, aux_dir) = run_all(&mut routed, p_dir, playing(0.0), n);
    assert!(
        energy(&mix_dir[BLOCK..]) == 0.0,
        "direct omits the instrument from 808 Mix"
    );
    assert!(energy(&aux_dir[tr808::VOICE_BD][BLOCK..]) > 0.0);
}

/// Muting a machine from the main Output leaves that machine's Mix bus intact.
#[test]
fn machine_direct_omits_from_main_keeps_mix() {
    let n = BLOCK * 8;
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut params = Params::default();
    params.p808[tr808::ENABLED] = 1;
    params.p808[tr808::BD_LEVEL] = 127;
    params.p808[tr808::ACCENT] = 127;
    params.direct_mix808 = 1;
    let (left, mix808, mix909, _) = run_all(&mut m, params, playing(0.0), n);
    assert!(
        energy(&mix808[BLOCK..]) > 0.0,
        "808 Mix still has the machine"
    );
    assert!(energy(&mix909[BLOCK..]) == 0.0);
    assert!(
        energy(&left[BLOCK..]) == 0.0,
        "direct mix omits the 808 from the main Output"
    );
}

#[test]
fn machine_direct_omits_909_from_main_keeps_mix() {
    let n = BLOCK * 8;
    let mut m = Modloader::new();
    m.set_step(1, 0, 0, tr909::BD as u8, 1);
    let mut params = Params::default();
    params.p909[tr909::ENABLED] = 1;
    params.p909[tr909::BD_LEVEL] = 127;
    params.p909[tr909::ACCENT] = 127;
    params.direct_mix909 = 1;
    let (left, mix808, mix909, _) = run_all(&mut m, params, playing(0.0), n);
    assert!(energy(&mix808[BLOCK..]) == 0.0);
    assert!(
        energy(&mix909[BLOCK..]) > 0.0,
        "909 Mix still has the machine"
    );
    assert!(
        energy(&left[BLOCK..]) == 0.0,
        "direct mix omits the 909 from the main Output"
    );
}

#[test]
fn params_c_layout() {
    assert_eq!(std::mem::size_of::<Params>(), 87);
    assert_eq!(std::mem::offset_of!(Params, mix808), 83);
    assert_eq!(std::mem::offset_of!(Params, mix909), 84);
    assert_eq!(std::mem::offset_of!(Params, direct_mix808), 85);
    assert_eq!(std::mem::offset_of!(Params, direct_mix909), 86);
}

#[test]
fn song_info_c_layout() {
    use crate::song::SongInfo;
    assert_eq!(std::mem::size_of::<SongInfo>(), 147);
    assert_eq!(std::mem::offset_of!(SongInfo, dist_enabled), 127);
    assert_eq!(std::mem::offset_of!(SongInfo, delay_send_909), 146);
}

/// Direct omits that instrument from the main sum; the aux bus still has it.
#[test]
fn direct_omitted_from_sum() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    let mut params = Params::default();
    params.p808[tr808::ENABLED] = 1;
    params.p808[tr808::BD_LEVEL] = 127;
    params.direct[tr808::VOICE_BD] = 1;

    let n = BLOCK * 4;
    let (left, aux) = run(&mut m, params, playing(0.0), n);
    assert!(
        energy(&aux[tr808::VOICE_BD][BLOCK..]) > 0.0,
        "direct still appears on its aux bus"
    );
    assert!(
        energy(&left[BLOCK..]) == 0.0,
        "direct is omitted from the main sum"
    );
}

/// 909 CH and OH cannot occupy the same step.
#[test]
fn ch_oh_exclusive() {
    let mut m = Modloader::new();
    m.set_step(1, 0, 2, tr909::CH as u8, 1);
    m.set_step(1, 0, 2, tr909::OH as u8, 2);
    assert_eq!(m.get_step(1, 0, 2, tr909::OH as u8), 2);
    assert_eq!(m.get_step(1, 0, 2, tr909::CH as u8), 0);
}

/// The DAT/ISO path stays set across load and reset.
#[test]
fn dat_path_survives_load_and_reset() {
    let mut m = Modloader::new();
    let p = Path::new("/tmp/ReBirth.iso");
    m.set_dat_path(p);
    assert_eq!(m.dat_path(), p.to_str().unwrap());
    assert_eq!(m.load(0), 1);
    assert_eq!(m.dat_path(), p.to_str().unwrap());
    m.reset();
    assert_eq!(m.dat_path(), p.to_str().unwrap());
}

/// Hosts call reset on aux-bus connect; step data and the current kit must survive.
#[test]
fn reset_keeps_pattern_and_kit() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 3, tr808::SD as u8, 1);
    m.set_step(1, 0, 5, tr909::LT as u8, 2);
    m.set_pat_length(0, 0, 12);
    m.reset();
    assert_eq!(m.current_mod(), 0);
    assert_eq!(m.get_step(0, 0, 3, tr808::SD as u8), 1);
    assert_eq!(m.get_step(1, 0, 5, tr909::LT as u8), 2);
    assert_eq!(m.get_pat_length(0, 0), 12);
}

fn both32(buf: &mut [u8], at: usize, v: u32) {
    buf[at..at + 4].copy_from_slice(&v.to_le_bytes());
    buf[at + 4..at + 8].copy_from_slice(&v.to_be_bytes());
}

fn both16(buf: &mut [u8], at: usize, v: u16) {
    buf[at..at + 2].copy_from_slice(&v.to_le_bytes());
    buf[at + 2..at + 4].copy_from_slice(&v.to_be_bytes());
}

fn iso_dir_record(name: &[u8], is_dir: bool, lba: u32, size: u32) -> Vec<u8> {
    let mut rec_len = 33 + name.len();
    if rec_len % 2 == 1 {
        rec_len += 1;
    }
    let mut r = vec![0u8; rec_len];
    r[0] = rec_len as u8;
    both32(&mut r, 2, lba);
    both32(&mut r, 10, size);
    r[25] = if is_dir { 2 } else { 0 };
    both16(&mut r, 28, 1);
    r[32] = name.len() as u8;
    r[33..33 + name.len()].copy_from_slice(name);
    r
}

/// A tiny ISO 9660 image with one file at the root (2048-byte sectors).
fn write_min_iso(path: &Path, file_name: &str, payload: &[u8]) {
    const SECTOR: usize = 2048;
    let ident = format!("{file_name};1").into_bytes();
    let file_rec = iso_dir_record(&ident, false, 19, payload.len() as u32);
    let dot = iso_dir_record(&[0], true, 18, SECTOR as u32);
    let dotdot = iso_dir_record(&[1], true, 18, SECTOR as u32);

    let mut image = vec![0u8; 20 * SECTOR];
    let pvd = &mut image[16 * SECTOR..][..SECTOR];
    pvd[0] = 1;
    pvd[1..6].copy_from_slice(b"CD001");
    pvd[6] = 1;
    both32(pvd, 80, 20);
    both16(pvd, 128, 2048);
    pvd[156..156 + dot.len()].copy_from_slice(&dot);

    let term = &mut image[17 * SECTOR..][..SECTOR];
    term[0] = 255;
    term[1..6].copy_from_slice(b"CD001");
    term[6] = 1;

    let root = &mut image[18 * SECTOR..][..SECTOR];
    let mut at = 0;
    for rec in [&dot, &dotdot, &file_rec] {
        root[at..at + rec.len()].copy_from_slice(rec);
        at += rec.len();
    }
    image[19 * SECTOR..][..payload.len()].copy_from_slice(payload);
    std::fs::write(path, image).unwrap();
}

/// A ReBirth 2.x song round-trips 808/909 patterns, knobs, mixer levels, shuffle, and the mod name.
#[test]
fn rbs_song_roundtrip_drums() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    m.set_step(0, 3, 4, tr808::SD as u8, 1);
    m.set_step(1, 7, 2, tr909::LT as u8, 2);
    m.set_pat_length(0, 1, 12);
    m.set_pat_shuffle(1, 7, 1);

    let mut info = crate::song::SongInfo::default();
    info.p808 = crate::Params::default().p808;
    info.p909 = crate::Params::default().p909;
    info.p808[tr808::BD_LEVEL] = 90;
    info.p909[tr909::FLAM] = 40;
    info.shuffle = 77;
    info.mix808 = 80;
    info.mix909 = 110;
    let name = b"Standard ReBirth";
    for (i, &b) in name.iter().enumerate() {
        info.mod_name[i] = b as i8;
    }

    let bytes = m.save_song(&info).expect("write rbs");
    let parsed = rbs::Song::parse(&bytes).expect("parse rbs");
    assert_eq!(parsed.head.version(), "4.2");
    assert_eq!(parsed.glob.mod_name, "Standard ReBirth");
    assert_eq!(parsed.mixer.channels[2].level, 80);
    assert_eq!(parsed.mixer.channels[3].level, 110);
    assert_eq!(
        parsed.tb303[0].params,
        orb_engine::tb303::DEFAULT_PARAMS[..9]
    );
    assert!(
        parsed.tb303[0]
            .patterns
            .iter()
            .all(|p| p.steps.iter().all(|s| s.rest == 1))
    );

    let mut loaded = Modloader::new();
    let got = loaded.load_song(&bytes).expect("load rbs");
    assert_eq!(got.p808[tr808::BD_LEVEL], 90);
    assert_eq!(got.p909[tr909::FLAM], 40);
    assert_eq!(got.shuffle, 77);
    assert_eq!(got.mix808, 80);
    assert_eq!(got.mix909, 110);
    let got_name = {
        let b = got.mod_name.map(|c| c as u8);
        let n = b.iter().position(|&c| c == 0).unwrap();
        String::from_utf8_lossy(&b[..n]).into_owned()
    };
    assert_eq!(got_name, "Standard ReBirth");
    assert_eq!(loaded.get_step(0, 0, 0, tr808::BD as u8), 1);
    assert_eq!(loaded.get_step(0, 3, 4, tr808::SD as u8), 1);
    assert_eq!(loaded.get_step(1, 7, 2, tr909::LT as u8), 2);
    assert_eq!(loaded.get_pat_length(0, 1), 12);
    assert_eq!(loaded.get_pat_shuffle(1, 7), 1);
    assert_eq!(loaded.get_step(0, 0, 1, tr808::BD as u8), 0);
    assert_eq!(got.dist_enabled, 0);
    assert_eq!(got.pcf_id, 0);
    assert_eq!(got.delay_send_808, 0);
}

/// Distortion / PCF / Delay mixer routing is kept on load and written back on save.
#[test]
fn rbs_song_keeps_effect_snapshot() {
    let mut file = rbs::Song::empty();
    file.dist.values = vec![1, 35, 62];
    file.pcf.values = vec![1, 20, 42, 50, 24, 90, 1];
    file.delay.values = vec![1, 8, 0, 70, 64];
    file.mixer.channels[2].dist = 1;
    file.mixer.channels[2].level = 80;
    file.mixer.pcf_id = 4;
    file.mixer.channels[3].delay_send = 22;

    let bytes = file.write().expect("write rbs");
    let mut m = Modloader::new();
    let info = m.load_song(&bytes).expect("load rbs");
    assert_eq!(info.dist_enabled, 1);
    assert_eq!(info.dist_amount, 35);
    assert_eq!(info.dist_shape, 62);
    assert_eq!(info.dist_808, 1);
    assert_eq!(info.dist_909, 0);
    assert_eq!(info.pcf_enabled, 1);
    assert_eq!(info.pcf_frequency, 20);
    assert_eq!(info.pcf_resonance, 42);
    assert_eq!(info.pcf_amount, 50);
    assert_eq!(info.pcf_wave, 24);
    assert_eq!(info.pcf_decay, 90);
    assert_eq!(info.pcf_mode, 1);
    assert_eq!(info.pcf_id, 4);
    assert_eq!(info.delay_enabled, 1);
    assert_eq!(info.delay_steps, 8);
    assert_eq!(info.delay_mode, 0);
    assert_eq!(info.delay_feedback, 70);
    assert_eq!(info.delay_pan, 64);
    assert_eq!(info.delay_send_808, 0);
    assert_eq!(info.delay_send_909, 22);
    assert_eq!(info.mix808, 80);

    let out = m.save_song(&info).expect("save rbs");
    let parsed = rbs::Song::parse(&out).expect("parse saved rbs");
    assert_eq!(parsed.dist.values, vec![1, 35, 62]);
    assert_eq!(parsed.pcf.values, vec![1, 20, 42, 50, 24, 90, 1]);
    assert_eq!(parsed.delay.values, vec![1, 8, 0, 70, 64]);
    assert_eq!(parsed.mixer.channels[2].dist, 1);
    assert_eq!(parsed.mixer.channels[3].dist, 0);
    assert_eq!(parsed.mixer.pcf_id, 4);
    assert_eq!(parsed.mixer.channels[2].delay_send, 0);
    assert_eq!(parsed.mixer.channels[3].delay_send, 22);
    assert_eq!(parsed.mixer.channels[2].level, 80);
}

/// Files before 4.2 convert 808 tom tunings the way ReBirth 2.x does (`v/2 + 32`).
#[test]
fn rbs_4_1_converts_808_tom_tune() {
    let mut song = rbs::Song::empty();
    song.head.minor = 1;
    song.tr808.params[10] = 20;
    let bytes = song.write().unwrap();
    let mut m = Modloader::new();
    let info = m.load_song(&bytes).expect("load 4.1");
    assert_eq!(info.p808[10], 20 / 2 + 32);
}

/// Garbage bytes are refused and leave patterns alone.
#[test]
fn rbs_invalid_file_keeps_patterns() {
    let mut m = Modloader::new();
    m.set_step(0, 0, 0, tr808::BD as u8, 1);
    assert!(m.load_song(&[0, 1, 2, 3, 4]).is_err());
    assert_eq!(m.get_step(0, 0, 0, tr808::BD as u8), 1);
}

/// `RB20FUL.DAT` is read from an ISO in place, without extracting the file.
#[test]
fn iso_serves_dat_in_place() {
    use std::io::Read;
    let dir = std::env::temp_dir().join(format!("orb-modloader-iso-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let iso = dir.join("rebirth.iso");
    let payload = b"ISO-DAT-PAYLOAD";
    write_min_iso(&iso, "RB20FUL.DAT", payload);
    let mut w = crate::iso::open_dat(&iso).expect("open DAT inside ISO");
    let mut got = Vec::new();
    w.read_to_end(&mut got).unwrap();
    assert_eq!(&got, payload);
    let _ = std::fs::remove_dir_all(&dir);
}
