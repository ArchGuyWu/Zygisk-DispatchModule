/// ShadowHook without-island proc-start: 20-byte patch + 20-byte backup.
pub const PATCH_BYTES: usize = 20;

pub fn build_trampoline(
    tramp: &mut [u8],
    backup: &[u8; PATCH_BYTES],
    old_base: u64,
    return_addr: u64,
    exec_base: u64,
) -> Option<usize> {
    let mut w = 0usize;
    for off in (0..PATCH_BYTES).step_by(4) {
        let insn = u32::from_le_bytes(backup[off..off + 4].try_into().ok()?);
        if !relocate_one(
            insn,
            old_base + off as u64,
            tramp,
            &mut w,
            tramp.len() - 16,
            exec_base,
        ) {
            return None;
        }
    }
    if w + 16 > tramp.len() {
        return None;
    }
    write_abs_ret_x17(&mut tramp[w..], return_addr);
    w += 16;
    Some(w)
}

fn write_u32(out: &mut [u8], v: u32) {
    out[..4].copy_from_slice(&v.to_le_bytes());
}

fn write_u64(out: &mut [u8], v: u64) {
    out[..8].copy_from_slice(&v.to_le_bytes());
}

/// BTI-safe return via X17 (matches ShadowHook absolute_jump_with_ret_ip).
fn write_abs_ret_x17(dst: &mut [u8], dest: u64) {
    write_u32(&mut dst[0..], 0x5800_0051);
    write_u32(&mut dst[4..], 0xD65F_0220);
    write_u64(&mut dst[8..], dest);
}

/// Entry patch: NOP + BR X17 (20-byte slot, preserves X0–X7 for detour args).
fn write_patch_to_detour(dst: &mut [u8], dest: u64) {
    write_u32(&mut dst[0..], 0xD503_201F);
    write_u32(&mut dst[4..], 0x5800_0051);
    write_u32(&mut dst[8..], 0xD61F_0220);
    write_u64(&mut dst[12..], dest);
}

fn sign_extend(v: u32, bits: u32) -> i64 {
    let m = 1u32 << (bits - 1);
    ((v ^ m).wrapping_sub(m)) as i32 as i64
}

/// Bytes after the inverted cond-branch occupied by `write_abs_ret_x17` (LDR+RET+imm64).
/// Skip imm is measured from the cond-branch PC, so target = PC + 4 + 16 = PC + 20 → imm=5.
const ABS_RET_SKIP_IMM: u32 = 5;

fn emit_cbz_abs(out: &mut [u8], w: &mut usize, cap: usize, insn: u32, dest: u64) -> bool {
    let inv = insn ^ (1 << 24);
    // Inverted CB(N)Z: when original condition is false, skip the absolute jump block.
    // Bug: imm=4 jumps +16 into the middle of the 64-bit literal → SIGILL at tramp+0x18.
    let skip_insn = (inv & !0x00FF_FFE0) | (ABS_RET_SKIP_IMM << 5);
    if *w + 4 + 16 > cap {
        return false;
    }
    write_u32(&mut out[*w..], skip_insn);
    *w += 4;
    write_abs_ret_x17(&mut out[*w..], dest);
    *w += 16;
    true
}

fn emit_bcond_abs(out: &mut [u8], w: &mut usize, cap: usize, insn: u32, dest: u64) -> bool {
    let cond = insn & 0xF;
    let inv = cond ^ 1;
    let skip_insn = 0x5400_0000 | (ABS_RET_SKIP_IMM << 5) | inv;
    if *w + 4 + 16 > cap {
        return false;
    }
    write_u32(&mut out[*w..], skip_insn);
    *w += 4;
    write_abs_ret_x17(&mut out[*w..], dest);
    *w += 16;
    true
}

fn emit_b_bl_abs(
    out: &mut [u8],
    w: &mut usize,
    cap: usize,
    insn: u32,
    old_pc: u64,
    dest: u64,
) -> bool {
    let is_bl = ((insn >> 26) & 0x3F) == 0x25;
    if is_bl {
        if *w + 32 > cap {
            return false;
        }
        write_u32(&mut out[*w..], 0x5800_00DE);
        write_u32(&mut out[*w + 4..], 0x5800_0070);
        write_u32(&mut out[*w + 8..], 0xD61F_0200);
        write_u32(&mut out[*w + 12..], 0xD503_201F);
        write_u64(&mut out[*w + 16..], dest);
        write_u64(&mut out[*w + 24..], old_pc + 4);
        *w += 32;
        return true;
    }
    if *w + 16 > cap {
        return false;
    }
    write_abs_ret_x17(&mut out[*w..], dest);
    *w += 16;
    true
}

fn emit_adr_adrp_abs(out: &mut [u8], w: &mut usize, cap: usize, insn: u32, old_pc: u64) -> bool {
    let rd = insn & 0x1F;
    let op = (insn >> 31) & 1;
    let imm = if op == 0 {
        let immlo = (insn >> 29) & 3;
        let immhi = (insn >> 5) & 0x7_FFFF;
        let off = sign_extend((immhi << 2) | immlo, 21) as u64;
        old_pc.wrapping_add(off)
    } else {
        let immlo = (insn >> 29) & 3;
        let immhi = (insn >> 5) & 0x7_FFFF;
        let off = (sign_extend((immhi << 2) | immlo, 21) as u64) << 12;
        (old_pc & !0xFFF) + off
    };
    if *w + 16 > cap {
        return false;
    }
    for i in 0..4 {
        let part = ((imm >> (16 * i)) & 0xFFFF) as u32;
        let m = if i == 0 {
            0xD280_0000 | (part << 5) | rd
        } else {
            0xF280_0000 | (i << 21) | (part << 5) | rd
        };
        write_u32(&mut out[*w..], m);
        *w += 4;
    }
    true
}

fn emit_ldr_literal_abs(out: &mut [u8], w: &mut usize, cap: usize, insn: u32, old_pc: u64) -> bool {
    let rt = insn & 0x1F;
    let opc = (insn >> 30) & 3;
    let imm19 = sign_extend((insn >> 5) & 0x7_FFFF, 19);
    let data_addr = old_pc.wrapping_add((imm19 << 2) as u64);
    if opc > 1 {
        return false;
    }
    if *w + 16 > cap {
        return false;
    }
    write_u32(&mut out[*w..], 0x5800_0050);
    *w += 4;
    let ldr = if opc == 1 {
        0xF940_0200 | (16 << 5) | rt
    } else {
        0xB940_0200 | (16 << 5) | rt
    };
    write_u32(&mut out[*w..], ldr);
    *w += 4;
    write_u64(&mut out[*w..], data_addr);
    *w += 8;
    true
}

fn emit_tbz_abs(out: &mut [u8], w: &mut usize, cap: usize, insn: u32, dest: u64) -> bool {
    let inv = insn ^ (1 << 24);
    // Same off-by-one as CBZ: skip must clear the full 16-byte abs-ret block (+20 from PC).
    let skip_insn = (inv & !0x7F_FFE0) | (ABS_RET_SKIP_IMM << 5);
    if *w + 4 + 16 > cap {
        return false;
    }
    write_u32(&mut out[*w..], skip_insn);
    *w += 4;
    write_abs_ret_x17(&mut out[*w..], dest);
    *w += 16;
    true
}

fn relocate_one(
    insn: u32,
    old_pc: u64,
    out: &mut [u8],
    w: &mut usize,
    cap: usize,
    tramp_base: u64,
) -> bool {
    let new_pc = tramp_base + *w as u64;

    if (insn & 0x7E00_0000) == 0x3400_0000 {
        let imm19 = sign_extend((insn >> 5) & 0x7_FFFF, 19);
        let dest = old_pc.wrapping_add((imm19 << 2) as u64);
        let new_off = (dest.wrapping_sub(new_pc) >> 2) as i64;
        if new_off >= -(1 << 18) && new_off < (1 << 18) {
            let ni = (insn & !0x00FF_FFE0) | (((new_off as u32) & 0x7_FFFF) << 5);
            if *w + 4 > cap {
                return false;
            }
            write_u32(&mut out[*w..], ni);
            *w += 4;
            return true;
        }
        return emit_cbz_abs(out, w, cap, insn, dest);
    }

    if (insn & 0x7E00_0000) == 0x3600_0000 {
        let imm14 = sign_extend((insn >> 5) & 0x3_FFFF, 14);
        let dest = old_pc.wrapping_add((imm14 << 2) as u64);
        let new_off = (dest.wrapping_sub(new_pc) >> 2) as i64;
        if new_off >= -(1 << 13) && new_off < (1 << 13) {
            let ni = (insn & !0x0007_FFE0) | (((new_off as u32) & 0x3_FFFF) << 5);
            if *w + 4 > cap {
                return false;
            }
            write_u32(&mut out[*w..], ni);
            *w += 4;
            return true;
        }
        return emit_tbz_abs(out, w, cap, insn, dest);
    }

    if (insn & 0xFF00_0010) == 0x5400_0000 {
        let imm19 = sign_extend((insn >> 5) & 0x7_FFFF, 19);
        let dest = old_pc.wrapping_add((imm19 << 2) as u64);
        let new_off = (dest.wrapping_sub(new_pc) >> 2) as i64;
        if new_off >= -(1 << 18) && new_off < (1 << 18) {
            let ni = (insn & !0x00FF_FFE0) | (((new_off as u32) & 0x7_FFFF) << 5);
            if *w + 4 > cap {
                return false;
            }
            write_u32(&mut out[*w..], ni);
            *w += 4;
            return true;
        }
        return emit_bcond_abs(out, w, cap, insn, dest);
    }

    if (insn & 0x7C00_0000) == 0x1400_0000 {
        let imm26 = sign_extend(insn & 0x03FF_FFFF, 26);
        let dest = old_pc.wrapping_add((imm26 << 2) as u64);
        let new_off = (dest.wrapping_sub(new_pc) >> 2) as i64;
        if new_off >= -(1 << 25) && new_off < (1 << 25) {
            let ni = (insn & 0xFC00_0000) | ((new_off as u32) & 0x03FF_FFFF);
            if *w + 4 > cap {
                return false;
            }
            write_u32(&mut out[*w..], ni);
            *w += 4;
            return true;
        }
        return emit_b_bl_abs(out, w, cap, insn, old_pc, dest);
    }

    if (insn & 0x1F00_0000) == 0x1000_0000 || (insn & 0x1F00_0000) == 0x9000_0000 {
        return emit_adr_adrp_abs(out, w, cap, insn, old_pc);
    }

    if (insn & 0x3B00_0000) == 0x1800_0000 {
        return emit_ldr_literal_abs(out, w, cap, insn, old_pc);
    }

    if *w + 4 > cap {
        return false;
    }
    write_u32(&mut out[*w..], insn);
    *w += 4;
    true
}

pub fn patch_abs_jump(detour: u64) -> [u8; PATCH_BYTES] {
    let mut patch = [0u8; PATCH_BYTES];
    write_patch_to_detour(&mut patch, detour);
    patch
}