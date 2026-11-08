use super::*;
use std::path::Path;

use orb_engine::tr808;

fn playing(ppq: f64) -> Playhead {
    Playhead {
        bpm: 120.0,
        ppq,
        playing: true,
    }
}

fn run(
    m: &mut Modloader,
    params: Params,
    playhead: Playhead,
    n: usize,
) -> (Vec<f32>, Vec<Vec<f32>>) {
    let mut left = vec![0.0f32; n];
    let mut right = vec![0.0f32; n];
    let mut aux_store = vec![vec![0.0f32; n]; AUX];
    let mut aux_refs: Vec<&mut [f32]> = aux_store.iter_mut().map(|v| v.as_mut_slice()).collect();
    m.process(&mut left, &mut right, &mut aux_refs, params, playhead);
    (left, aux_store)
}

fn energy(buf: &[f32]) -> f32 {
    buf.iter().map(|x| x.abs()).sum()
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
    assert_eq!(straight, amount_off, "amount 0 is identity even with the flag on");
    assert!(
        swung > straight + BLOCK,
        "flag + amount 127 should delay the offbeat (straight {straight}, swung {swung})"
    );
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
