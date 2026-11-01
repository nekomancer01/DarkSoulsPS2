// exe-dearxan: offline Arxan decryption of a copy of DarkSoulsRemastered.exe.
//
// Usage: exe-dearxan <in.exe> <out-dir>
// Maps the PE, runs dearxan's stub analysis, writes every decrypted region into the image and repeats until a
// pass finds no new stubs (some stubs are themselves encrypted). Never runs game code.
// Outputs in <out-dir>:
//   DarkSoulsRemastered.decrypted.exe  PE whose file layout equals the memory layout (raw offset = RVA)
//   stubs.tsv                          one line per stub: test_rsp_va context_pop_va return_gadget_va regions kind
//   regions.tsv                        one line per decrypted region: va size pass stub_va
use std::{collections::HashSet, fmt::Write as _, fs, path::PathBuf};

use dearxan::analysis::{StubInfo, analyze_all_stubs, encryption};
use pelite::pe64::{Pe, PeFile, PeObject, PeView};

const BASE: u64 = 0x1_4000_0000;

fn rd32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn wr32(b: &mut [u8], o: usize, v: u32) {
    b[o..o + 4].copy_from_slice(&v.to_le_bytes());
}

// Same as dearxan-test-utils pe_file_to_view (pelite's own to_view is broken).
fn map_image(pe: PeFile) -> Vec<u8> {
    let oh = pe.optional_header();
    let mut img = vec![0u8; oh.SizeOfImage as usize];
    let raw = pe.image();
    let hs = oh.SizeOfHeaders as usize;
    img[..hs].copy_from_slice(&raw[..hs]);
    for s in pe.section_headers() {
        let va = s.VirtualAddress as usize;
        let n = (s.SizeOfRawData.min(s.VirtualSize)) as usize;
        let src = s.PointerToRawData as usize;
        if src + n <= raw.len() && va + n <= img.len() {
            img[va..va + n].copy_from_slice(&raw[src..src + n]);
        }
    }
    img
}

// Rewrite the section table so the mapped image is also a valid file (PointerToRawData = VirtualAddress).
fn image_to_file(mut img: Vec<u8>) -> Vec<u8> {
    let pe = rd32(&img, 0x3c) as usize;
    let nsec = u16::from_le_bytes([img[pe + 6], img[pe + 7]]) as usize;
    let opt = u16::from_le_bytes([img[pe + 20], img[pe + 21]]) as usize;
    let sec0 = pe + 24 + opt;
    let size = img.len() as u32;
    for i in 0..nsec {
        let h = sec0 + i * 40;
        let va = rd32(&img, h + 12);
        let next = if i + 1 < nsec { rd32(&img, h + 40 + 12) } else { size };
        wr32(&mut img, h + 16, next - va);
        wr32(&mut img, h + 20, va);
    }
    img
}

fn main() {
    simplelog::TermLogger::init(
        log::LevelFilter::Info,
        simplelog::Config::default(),
        simplelog::TerminalMode::Stderr,
        simplelog::ColorChoice::Never,
    )
    .unwrap();
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: exe-dearxan <in.exe> <out-dir>");
        std::process::exit(2);
    }
    let out = PathBuf::from(&args[2]);
    fs::create_dir_all(&out).unwrap();
    let raw = fs::read(&args[1]).expect("read exe");
    let mut img = map_image(PeFile::from_bytes(&raw).expect("PE"));

    let mut seen: HashSet<u64> = HashSet::new();
    let mut stubs_tsv = String::from("test_rsp_va\tcontext_pop_va\treturn_gadget_va\tregions\tkind\tpass\n");
    let mut regions_tsv = String::from("va\tsize\tpass\tstub_va\n");
    let (mut n_err, mut n_bytes) = (0usize, 0usize);
    for pass in 1..=8 {
        let view = PeView::from_bytes(&img).expect("mapped view");
        let results = analyze_all_stubs(view);
        let mut new: Vec<StubInfo> = Vec::new();
        for r in results {
            match r {
                Ok(si) if seen.insert(si.test_rsp_va) => new.push(si),
                Ok(_) => {}
                Err(e) => {
                    if pass == 1 {
                        n_err += 1;
                        log::warn!("{e}");
                    }
                }
            }
        }
        log::info!("pass {pass}: {} new stubs", new.len());
        if new.is_empty() {
            break;
        }
        for si in &new {
            let (n, kind) = match &si.encrypted_regions {
                Some(l) => (l.len(), format!("{:?}", l.kind)),
                None => (0, "-".into()),
            };
            let rg = si.return_gadget.as_ref().map_or(0, |g| g.address);
            writeln!(stubs_tsv, "{:x}\t{:x}\t{:x}\t{n}\t{kind}\t{pass}", si.test_rsp_va, si.context_pop_va, rg).unwrap();
        }
        // Region lists keep their stub for the log; conflicts between lists are resolved by dearxan.
        let lists = encryption::apply_relocs_and_resolve_conflicts(
            new.iter().filter_map(|si| si.encrypted_regions.as_ref()),
            view,
            Some(BASE),
        )
        .expect("relocs");
        let mut writes = Vec::new();
        for l in &lists {
            for r in &l.regions {
                let bytes = l.decrypted_stream[r.stream_offset..r.stream_offset + r.size].to_vec();
                writes.push((r.rva as usize, bytes));
            }
        }
        for (rva, bytes) in writes {
            let stub = new
                .iter()
                .find(|si| si.encrypted_regions.as_ref().is_some_and(|l| l.regions.iter().any(|x| x.rva as usize == rva)))
                .map_or(0, |si| si.test_rsp_va);
            writeln!(regions_tsv, "{:x}\t{:x}\t{pass}\t{stub:x}", BASE + rva as u64, bytes.len()).unwrap();
            n_bytes += bytes.len();
            img[rva..rva + bytes.len()].copy_from_slice(&bytes);
        }
    }
    log::info!("{} stubs, {} analysis errors, {} bytes decrypted", seen.len(), n_err, n_bytes);
    fs::write(out.join("stubs.tsv"), stubs_tsv).unwrap();
    fs::write(out.join("regions.tsv"), regions_tsv).unwrap();
    fs::write(out.join("DarkSoulsRemastered.decrypted.exe"), image_to_file(img)).unwrap();
}
