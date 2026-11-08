//! ISO 9660 reader for a ReBirth 2.x CD image: locate `RB20FUL.DAT` and expose it as
//! `Read + Seek` without extracting the 128 MB file. 2048-byte sectors only (the same
//! layout OpenRB-338's `orb-cli` reads). Joliet names are preferred when present.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

const SECTOR: u64 = 2048;
const MAX_DIR: u64 = 16 << 20;
const DAT_NAME: &str = "RB20FUL.DAT";

#[derive(Debug, Clone)]
struct Entry {
    name: String,
    is_dir: bool,
    offset: u64,
    size: u64,
}

/// A byte range of the image as its own `Read + Seek`. It owns a handle and seeks
/// before every read.
pub struct Window {
    file: File,
    start: u64,
    len: u64,
    pos: u64,
}

impl Read for Window {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let left = self.len.saturating_sub(self.pos);
        let n = buf.len().min(left.min(usize::MAX as u64) as usize);
        if n == 0 {
            return Ok(0);
        }
        self.file.seek(SeekFrom::Start(self.start + self.pos))?;
        let n = self.file.read(&mut buf[..n])?;
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for Window {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let p = match to {
            SeekFrom::Start(p) => Some(p),
            SeekFrom::End(d) => self.len.checked_add_signed(d),
            SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        self.pos = p.ok_or_else(|| {
            std::io::Error::new(std::io::ErrorKind::InvalidInput, "seek before the start")
        })?;
        Ok(self.pos)
    }
}

struct Iso {
    file: File,
    block: u64,
    root: (u64, u64),
    joliet: bool,
}

fn le32(b: &[u8], at: usize) -> u64 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]]) as u64
}

impl Iso {
    fn open(path: &Path) -> Result<Iso, String> {
        let mut file =
            File::open(path).map_err(|e| format!("{}: cannot read: {e}", path.display()))?;
        let not_iso = |why: &str| {
            format!(
                "{}: not an ISO 9660 image ({why}); use a 2048-byte-sector .iso of the ReBirth CD",
                path.display()
            )
        };
        let mut primary: Option<[u8; 2048]> = None;
        let mut joliet: Option<[u8; 2048]> = None;
        let mut sector = 16u64;
        loop {
            let mut d = [0u8; 2048];
            file.seek(SeekFrom::Start(sector * SECTOR))
                .and_then(|_| file.read_exact(&mut d))
                .map_err(|_| {
                    if sector == 16 {
                        not_iso("too short")
                    } else {
                        not_iso("no volume descriptor terminator")
                    }
                })?;
            if &d[1..6] != b"CD001" {
                return Err(not_iso("no CD001 volume descriptor at sector 16"));
            }
            match d[0] {
                1 if primary.is_none() => primary = Some(d),
                2 if matches!(&d[88..91], b"%/@" | b"%/C" | b"%/E") => joliet = Some(d),
                255 => break,
                _ => {}
            }
            sector += 1;
            if sector > 16 + 32 {
                return Err(not_iso("no volume descriptor terminator"));
            }
        }
        let (d, is_joliet) = match (joliet, primary) {
            (Some(d), _) => (d, true),
            (None, Some(d)) => (d, false),
            _ => return Err(not_iso("no primary volume descriptor")),
        };
        let block = u16::from_le_bytes([d[128], d[129]]) as u64;
        if block != SECTOR {
            return Err(not_iso(&format!("logical block size {block}")));
        }
        Ok(Iso {
            file,
            block,
            root: (le32(&d, 158) * block, le32(&d, 166)),
            joliet: is_joliet,
        })
    }

    fn name(&self, raw: &[u8]) -> String {
        if self.joliet {
            let u: Vec<u16> = (0..raw.len() / 2)
                .map(|i| u16::from_be_bytes([raw[2 * i], raw[2 * i + 1]]))
                .collect();
            let s = String::from_utf16_lossy(&u);
            s.split(';').next().unwrap_or("").to_string()
        } else {
            let s: String = raw.iter().map(|&b| b as char).collect();
            let s = s.split(';').next().unwrap_or("");
            s.strip_suffix('.').unwrap_or(s).to_string()
        }
    }

    fn read_dir(&mut self, (offset, size): (u64, u64)) -> Result<Vec<Entry>, String> {
        if size > MAX_DIR {
            return Err(format!("directory of {size} bytes"));
        }
        let mut buf = vec![0u8; size as usize];
        self.file
            .seek(SeekFrom::Start(offset))
            .and_then(|_| self.file.read_exact(&mut buf))
            .map_err(|e| format!("cannot read a directory: {e}"))?;
        let mut v = Vec::new();
        let mut at = 0usize;
        while at < buf.len() {
            let len = buf[at] as usize;
            if len == 0 {
                at = (at / self.block as usize + 1) * self.block as usize;
                continue;
            }
            if len < 34 || at + len > buf.len() || 33 + buf[at + 32] as usize > len {
                return Err("a damaged directory record".into());
            }
            let r = &buf[at..at + len];
            at += len;
            let raw = &r[33..33 + r[32] as usize];
            if raw == [0] || raw == [1] {
                continue;
            }
            if r[25] & 0x80 != 0 {
                return Err(format!(
                    "{}: a multi-extent file, which is not read",
                    self.name(raw)
                ));
            }
            v.push(Entry {
                name: self.name(raw),
                is_dir: r[25] & 2 != 0,
                offset: le32(r, 2) * self.block,
                size: le32(r, 10),
            });
        }
        Ok(v)
    }

    fn window(&self, e: &Entry) -> Result<Window, String> {
        Ok(Window {
            file: self
                .file
                .try_clone()
                .map_err(|e| format!("cannot reopen the image: {e}"))?,
            start: e.offset,
            len: e.size,
            pos: 0,
        })
    }
}

/// `RB20FUL.DAT` inside `path`, read in place. Looks at the root, then one directory down.
pub fn open_dat(path: &Path) -> Result<Window, String> {
    let mut iso = Iso::open(path)?;
    let root = iso.read_dir(iso.root)?;
    if let Some(e) = root
        .iter()
        .find(|e| !e.is_dir && e.name.eq_ignore_ascii_case(DAT_NAME))
    {
        return iso.window(e);
    }
    let dirs: Vec<Entry> = root.into_iter().filter(|e| e.is_dir).collect();
    for dir in dirs {
        let entries = iso.read_dir((dir.offset, dir.size))?;
        if let Some(e) = entries
            .iter()
            .find(|e| !e.is_dir && e.name.eq_ignore_ascii_case(DAT_NAME))
        {
            return iso.window(e);
        }
    }
    Err(format!("{}: no RB20FUL.DAT in the image", path.display()))
}
