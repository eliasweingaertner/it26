#!/usr/bin/env python3
"""
gen_import_tests.py -- generate minimal, valid test modules for the
import feature (specs/007): one looping sine sample, a couple of
4-channel patterns with notes/effects, in each format IT loaded
natively (S3M, MOD, MTM, 669, XM).

Usage: python gen_import_tests.py <outdir>
"""
import math
import struct
import sys

SR = 256                            # sample frames


def sine8(signed):
    out = bytearray()
    for i in range(SR):
        v = int(100 * math.sin(2 * math.pi * i / 32))
        out.append((v & 0xFF) if signed else ((v + 128) & 0xFF))
    return bytes(out)


def gen_s3m(path):
    # header
    hdr = bytearray(0x60)
    hdr[0:12] = b'IMPORT S3M\x00\x00'
    hdr[0x1C] = 0x1A
    hdr[0x1D] = 16                  # type: ST3 module
    struct.pack_into('<HHH', hdr, 0x20, 2, 1, 1)   # ord/ins/pat
    struct.pack_into('<H', hdr, 0x26, 0)           # flags
    struct.pack_into('<H', hdr, 0x28, 0x1320)      # cwt/v (ST3.20)
    struct.pack_into('<H', hdr, 0x2A, 2)           # ffi: unsigned
    hdr[0x2C:0x30] = b'SCRM'
    hdr[0x30] = 32                  # global volume 0..64
    hdr[0x31] = 6                   # speed
    hdr[0x32] = 125                 # tempo
    hdr[0x33] = 0xB0                # master volume | stereo
    for c in range(4):
        hdr[0x40 + c] = [0, 8, 1, 9][c]   # L L R R channel settings
    for c in range(4, 32):
        hdr[0x40 + c] = 255         # unused

    orders = bytes([0, 255])
    # layout: header, orders, parapointers (1 ins + 1 pat)
    off = 0x60 + len(orders) + 2 + 2
    off = (off + 15) & ~15
    inspara = off // 16
    smphdr_off = off

    smpdata_off = (smphdr_off + 0x50 + 15) & ~15
    pat_off = (smpdata_off + SR + 15) & ~15

    ins = bytearray(0x50)
    ins[0] = 1                      # type: sample
    ins[1:12] = b'TEST.SMP\x00\x00\x00'
    memseg = smpdata_off // 16
    ins[0x0D] = (memseg >> 16) & 0xFF
    struct.pack_into('<H', ins, 0x0E, memseg & 0xFFFF)
    struct.pack_into('<I', ins, 0x10, SR)       # length
    struct.pack_into('<I', ins, 0x14, 0)        # loop begin
    struct.pack_into('<I', ins, 0x18, SR)       # loop end
    ins[0x1C] = 48                              # volume
    ins[0x1F] = 1                               # flags: loop
    struct.pack_into('<I', ins, 0x20, 8363)     # c2spd
    ins[0x30:0x3C] = b'import test\x00'
    ins[0x4C:0x50] = b'SCRS'

    # one 64-row pattern: C-4 ins1 vol48 on ch0 row0, D-4 ch1 row4 + D01
    rows = []
    ev = bytearray()
    ev += bytes([0 | 32 | 64, 0x30, 1, 48])     # ch0: note C-4(oct3?)
    ev += bytes([0])                            # end row 0
    rows.append(bytes(ev))
    for _ in range(3):
        rows.append(bytes([0]))
    ev = bytearray()
    ev += bytes([1 | 32 | 128, 0x32, 1, ord('D') - ord('@'), 0x01])
    ev += bytes([0])
    rows.append(bytes(ev))
    for _ in range(64 - 5):
        rows.append(bytes([0]))
    pdata = b''.join(rows)
    packed = struct.pack('<H', len(pdata) + 2) + pdata

    out = bytearray()
    out += hdr
    out += orders
    out += struct.pack('<H', inspara)
    out += struct.pack('<H', pat_off // 16)
    out += b'\x00' * (smphdr_off - len(out))
    out += ins
    out += b'\x00' * (smpdata_off - len(out))
    out += sine8(False)             # unsigned data
    out += b'\x00' * (pat_off - len(out))
    out += packed
    open(path, 'wb').write(out)


def gen_mod(path):
    out = bytearray()
    out += b'IMPORT MOD'.ljust(20, b'\x00')
    for i in range(31):
        sh = bytearray(30)
        if i == 0:
            sh[0:10] = b'import smp'
            struct.pack_into('>H', sh, 22, SR // 2)  # length in words
            sh[24] = 0                               # finetune
            sh[25] = 48                              # volume
            struct.pack_into('>H', sh, 26, 0)        # repeat start
            struct.pack_into('>H', sh, 28, SR // 2)  # repeat len
        out += sh
    out += bytes([2, 127])          # song length, restart
    orders = bytearray(128)
    orders[1] = 1
    out += orders
    out += b'M.K.'
    for p in range(2):
        pat = bytearray(4 * 4 * 64)
        # row0 ch0: period 428 (C-2), sample 1, effect C40 (volume)
        per = 428
        pat[0] = (1 & 0xF0) | (per >> 8)
        pat[1] = per & 0xFF
        pat[2] = (1 << 4) | 0x0C
        pat[3] = 0x30
        out += pat
    out += sine8(True)
    open(path, 'wb').write(out)


def gen_mtm(path):
    trk = bytearray(192)
    # row0: pitch 25, ins 1, effect C val 0x30
    pitch, ins, fx, fv = 25, 1, 0x0C, 0x30
    trk[0] = (pitch << 2) | (ins >> 4)
    trk[1] = ((ins & 0x0F) << 4) | fx
    trk[2] = fv
    comment = b'MTM import test comment'.ljust(80, b'\x00')

    out = bytearray()
    out += b'MTM\x10'
    out += b'IMPORT MTM'.ljust(20, b'\x00')
    out += struct.pack('<H', 1)     # ntracks
    out += bytes([0])               # last pattern
    out += bytes([0])               # last order
    out += struct.pack('<H', len(comment))
    out += bytes([1])               # samples
    out += bytes([0])               # attribute
    out += bytes([64])              # rows per pattern
    out += bytes([4])               # channels
    pans = bytearray(32)
    for c in range(4):
        pans[c] = [0, 15, 3, 12][c]
    out += pans
    sh = bytearray(37)
    sh[0:10] = b'import smp'
    struct.pack_into('<I', sh, 22, SR)      # length
    struct.pack_into('<I', sh, 26, 0)       # loop start
    struct.pack_into('<I', sh, 30, SR)      # loop end
    sh[34] = 0                              # finetune
    sh[35] = 48                             # volume
    sh[36] = 0                              # 8-bit
    out += sh
    out += bytearray(128)                   # orders (pattern 0)
    out += trk                              # track 1
    seq = bytearray(64)                     # pattern 0 track sequence
    struct.pack_into('<H', seq, 0, 1)       # ch0 -> track 1
    out += seq
    out += comment
    out += sine8(False)                     # unsigned
    open(path, 'wb').write(out)


def gen_669(path):
    out = bytearray()
    out += b'if'
    out += b'669 import test'.ljust(108, b' ')
    out += bytes([1])               # NOS
    out += bytes([1])               # NOP
    out += bytes([0])               # loop order
    orders = bytearray(128)
    orders[0] = 0
    for i in range(1, 128):
        orders[i] = 0xFF
    out += orders
    tempos = bytearray(128)
    tempos[0] = 4
    out += tempos
    breaks = bytearray(128)
    breaks[0] = 63
    out += breaks
    assert len(out) == 0x1F1, len(out)
    sh = bytearray(25)
    sh[0:10] = b'import smp'
    struct.pack_into('<I', sh, 13, SR)
    struct.pack_into('<I', sh, 17, 0)
    struct.pack_into('<I', sh, 21, SR)
    out += sh
    pat = bytearray(0x600)
    for i in range(0, 0x600, 3):
        pat[i] = 0xFF
        pat[i + 2] = 0xFF
    # row0 ch0: note 24 (C-?), ins 0, vol 12
    pat[0] = 24 << 2
    pat[1] = 12
    pat[2] = 0xFF
    out += pat
    out += sine8(False)
    open(path, 'wb').write(out)


def gen_xm(path):
    out = bytearray()
    out += b'Extended Module: '
    out += b'IMPORT XM'.ljust(20, b'\x00')
    out += bytes([0x1A])
    out += b'gen_import_tests'.ljust(20, b'\x00')
    out += struct.pack('<H', 0x0104)
    hdr = struct.pack('<IHHHHHHHH', 20 + 256, 1, 0, 4, 1, 1, 1, 6, 125)
    out += hdr
    out += bytes([0]) + bytearray(255)      # order table
    # pattern: 9-byte header + packed data
    rows = 64
    pdata = bytearray()
    # row0 ch0: full cell note C-4(49?), ins 1, vol 0x40, effect 0
    pdata += bytes([49, 1, 0x40, 0, 0])
    for _ in range(3):                      # ch1..3 empty
        pdata += bytes([0x80])
    for _ in range(rows - 1):
        for _ in range(4):
            pdata += bytes([0x80])
    out += struct.pack('<IBHH', 9, 0, rows, len(pdata))
    out += pdata
    # instrument
    ihdr = bytearray(263)
    struct.pack_into('<I', ihdr, 0, 263)
    ihdr[4:14] = b'import ins'
    ihdr[26] = 0
    struct.pack_into('<H', ihdr, 27, 1)     # one sample
    struct.pack_into('<H', ihdr, 29, 40)    # sample header size
    # vol env: 2 points (0,64) (32,32), on
    struct.pack_into('<HH', ihdr, 129, 0, 64)
    struct.pack_into('<HH', ihdr, 133, 32, 32)
    ihdr[225] = 2                           # num vol points
    ihdr[233] = 1                           # vol type: on
    struct.pack_into('<H', ihdr, 239, 128)  # fadeout
    out += ihdr
    sh = bytearray(40)
    struct.pack_into('<I', sh, 0, SR)       # length (bytes)
    struct.pack_into('<I', sh, 4, 0)        # loop start
    struct.pack_into('<I', sh, 8, SR)       # loop length
    sh[12] = 48                             # volume
    sh[13] = 0                              # finetune
    sh[14] = 1                              # type: forward loop, 8 bit
    sh[15] = 128                            # pan
    sh[16] = 0                              # relnote
    sh[18:28] = b'import smp'
    out += sh
    # delta-encoded signed data
    raw = sine8(True)
    prev = 0
    delta = bytearray()
    for b in raw:
        v = b if b < 128 else b - 256
        delta.append((v - prev) & 0xFF)
        prev = v
    out += delta
    open(path, 'wb').write(out)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    gen_s3m(outdir + '/import_test.s3m')
    gen_mod(outdir + '/import_test.mod')
    gen_mtm(outdir + '/import_test.mtm')
    gen_669(outdir + '/import_test.669')
    gen_xm(outdir + '/import_test.xm')
    print('wrote import_test.{s3m,mod,mtm,669,xm} to', outdir)


if __name__ == '__main__':
    main()
