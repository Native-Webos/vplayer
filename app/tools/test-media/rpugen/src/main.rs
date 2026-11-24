// SPDX-License-Identifier: GPL-3.0-or-later
// rpugen FRAMES OUT: Profile 8.1 RPUs, one per frame, as complete HEVC UNSPEC62 NAL units
// (0x7C01 header, emulation prevention) each behind a 4-byte start code.
use dolby_vision::rpu::generate::{GenerateConfig, GenerateProfile, VideoShot};
use std::io::Write;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let frames: usize = args[1].parse().unwrap();
    let config = GenerateConfig {
        profile: GenerateProfile::Profile81,
        length: frames,
        shots: vec![VideoShot { start: 0, duration: frames, ..Default::default() }],
        ..Default::default()
    };
    let rpus = config.generate_rpu_list().unwrap();
    let mut out = std::fs::File::create(&args[2]).unwrap();
    for rpu in &rpus {
        let nal = rpu.write_hevc_unspec62_nalu().unwrap();
        out.write_all(&[0, 0, 0, 1]).unwrap();
        out.write_all(&nal).unwrap();
    }
    eprintln!("{} RPUs", rpus.len());
}
