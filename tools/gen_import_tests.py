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


def gen_xi(path):
    """minimal .XI instrument (sample library feature 006)"""
    out = bytearray()
    out += b'Extended Instrument: '              # 21
    out += b'lib test xi'.ljust(22, b' ')        # name @21
    out += bytes([0x1A])                         # @43
    out += b'gen_import_tests'.ljust(20, b'\x00')  # tracker @44
    out += struct.pack('<H', 0x0102)             # version @64
    assert len(out) == 66
    out += bytearray(96)                         # note->sample table @66
    env = bytearray(48)                          # vol env points @162
    struct.pack_into('<HH', env, 0, 0, 64)
    struct.pack_into('<HH', env, 4, 40, 32)
    out += env
    out += bytearray(48)                         # pan env points @210
    tail = bytearray(298 - 258)
    tail[258 - 258] = 2                          # num vol points
    tail[259 - 258] = 0                          # num pan points
    tail[266 - 258] = 1                          # vol flags: on
    struct.pack_into('<H', tail, 272 - 258, 128)   # fadeout
    struct.pack_into('<H', tail, 296 - 258, 1)     # NoS
    out += tail
    assert len(out) == 298
    sh = bytearray(40)                           # XM sample header
    struct.pack_into('<I', sh, 0, SR)
    struct.pack_into('<I', sh, 4, 0)
    struct.pack_into('<I', sh, 8, SR)
    sh[12] = 48
    sh[14] = 1                                   # forward loop, 8 bit
    sh[15] = 128
    sh[18:28] = b'xi sample '
    out += sh
    raw = sine8(True)                            # delta-encoded signed
    prev = 0
    for b in raw:
        v = b if b < 128 else b - 256
        out.append((v - prev) & 0xFF)
        prev = v
    open(path, 'wb').write(out)


def gen_ptm(path):
    """minimal .PTM (Poly Tracker) -- sample headers @608, 80 bytes"""
    hdr = bytearray(608)
    hdr[0:10] = b'IMPORT PTM'
    hdr[28] = 3                                  # version
    struct.pack_into('<H', hdr, 32, 1)           # norders?
    struct.pack_into('<H', hdr, 34, 1)           # nsamples
    hdr[44:48] = b'PTMF'
    sh = bytearray(80)
    data_off = 608 + 80
    sh[0] = 1 | 0x04                             # sample + loop
    sh[1:9] = b'PTM.SMP\x00'
    sh[13] = 48                                  # volume
    struct.pack_into('<H', sh, 14, 8363)         # C4 speed
    struct.pack_into('<I', sh, 18, data_off)     # data offset
    struct.pack_into('<I', sh, 22, SR)           # length (bytes)
    struct.pack_into('<I', sh, 26, 0)            # loop begin
    struct.pack_into('<I', sh, 30, SR)           # loop end
    sh[48:58] = b'ptm sample'
    sh[76:80] = b'PTMS'
    # PTM data is signed byte-delta
    raw = sine8(True)
    prev = 0
    delta = bytearray()
    for b in raw:
        v = b if b < 128 else b - 256
        delta.append((v - prev) & 0xFF)
        prev = v
    open(path, 'wb').write(bytes(hdr) + bytes(sh) + bytes(delta))


def gen_far(path):
    """minimal .FAR (Farandole): text len 0, zero pattern sizes,
    sample map + 48-byte header + data"""
    out = bytearray(869)
    out[0:4] = b'FAR\xFE'
    out[4:14] = b'IMPORT FAR'
    struct.pack_into('<H', out, 96, 0)           # text length
    # bytes 98..868: order list / pattern sizes, all zero
    smap = bytearray(8)
    smap[0] = 1                                  # sample 0 present
    out += smap
    sh = bytearray(48)
    sh[0:10] = b'far sample'
    struct.pack_into('<I', sh, 32, SR)           # length
    sh[36] = 0                                   # finetune
    sh[37] = 48                                  # volume (ignored by IT)
    struct.pack_into('<I', sh, 38, 0)            # loop start
    struct.pack_into('<I', sh, 42, SR)           # loop end
    struct.pack_into('<H', sh, 46, 0x0800)       # loop on (bit 3 hi byte)
    out += sh
    out += sine8(True)                           # signed 8-bit
    open(path, 'wb').write(out)


def gen_krz(path):
    """minimal .KRZ (Kurzweil): 32-byte header, one 0x98 block, 16-bit
    big-endian data"""
    frames = SR
    hdr_blocks = 32 + 1024                       # data starts here
    out = bytearray(32)
    struct.pack_into('>I', out, 4, hdr_blocks)   # sample data offset
    blk = bytearray(1024)
    blk[4] = 0x98                                # sample object
    struct.pack_into('>H', blk, 6, 1017)         # block size (rounds to 1024)
    struct.pack_into('>H', blk, 8, 40)           # header at blk+40+20
    blk[10:20] = b'krz sample'
    h = 60                                       # 40 + 20
    blk[h + 1] = 0x80                            # loop off
    struct.pack_into('>I', blk, h + 8, 0)        # start
    struct.pack_into('>I', blk, h + 16, 0)       # loop begin
    struct.pack_into('>I', blk, h + 20, frames)  # loop end (= length)
    struct.pack_into('>I', blk, h + 28, 45351)   # period -> ~22050 Hz
    out += blk
    for i in range(frames):                      # 16-bit BE sine
        v = int(20000 * math.sin(2 * math.pi * i / 32))
        out += struct.pack('>h', v)
    open(path, 'wb').write(out)


def gen_pat(path):
    """minimal .PAT (Gravis patch): file+instrument+layer headers,
    one wave"""
    out = bytearray(239)
    out[0:22] = b'GF1PATCH110\x00ID#000002\x00'
    out[129 + 2:129 + 2 + 10] = b'pat instr\x00'
    out[129 + 63 + 6] = 1                        # wave count
    w = bytearray(96)
    w[0:7] = b'wave1\x00\x00'
    struct.pack_into('<I', w, 8, SR)             # data size
    struct.pack_into('<I', w, 12, 0)             # loop start
    struct.pack_into('<I', w, 16, SR)            # loop end (= IT length)
    struct.pack_into('<H', w, 20, 8363)          # sample rate
    w[55] = 0x04 | 0x02                          # loop on, unsigned
    out += w
    out += sine8(False)                          # unsigned 8-bit
    open(path, 'wb').write(out)


def wav_header(nch, rate, bits, dsize, tag=1):
    """canonical 44-byte RIFF/WAVE header"""
    ba = nch * bits // 8
    hdr = b'RIFF' + struct.pack('<I', 36 + dsize) + b'WAVEfmt '
    hdr += struct.pack('<IHHIIHH', 16, tag, nch, rate, rate * ba, ba, bits)
    hdr += b'data' + struct.pack('<I', dsize)
    return hdr


def wav_ramp16(i):
    """deterministic 16-bit ramp; the selftest recomputes this in C"""
    return (i << 7) - 16384


def gen_wav8(path):
    """mono 8-bit unsigned PCM, 22050 Hz: byte i = (i*7)&0xFF"""
    data = bytes((i * 7) & 0xFF for i in range(SR))
    open(path, 'wb').write(wav_header(1, 22050, 8, len(data)) + data)


def gen_wav16(path):
    """mono 16-bit signed PCM, 44100 Hz: the ramp"""
    data = b''.join(struct.pack('<h', wav_ramp16(i)) for i in range(SR))
    open(path, 'wb').write(wav_header(1, 44100, 16, len(data)) + data)


def gen_wavst(path):
    """stereo 16-bit PCM, 44100 Hz: left = the ramp, right = ~left --
    the library loads the left channel only (feature 008)"""
    data = b''.join(struct.pack('<hh', wav_ramp16(i), ~wav_ramp16(i))
                    for i in range(SR))
    open(path, 'wb').write(wav_header(2, 44100, 16, len(data)) + data)


def gen_wavf(path):
    """negative fixture: float WAV (wFormatTag=3) must be refused"""
    data = b''.join(struct.pack('<f', 0.25) for _ in range(SR))
    open(path, 'wb').write(wav_header(1, 44100, 32, len(data), tag=3) + data)


def gen_wav24(path):
    """negative fixture: 24-bit PCM must be refused"""
    data = b''.join(struct.pack('<i', wav_ramp16(i) << 8)[0:3]
                    for i in range(SR))
    open(path, 'wb').write(wav_header(1, 44100, 24, len(data)) + data)


def gen_iff(path):
    """IFF 8SVX (feature 013): NAME + VHDR + BODY chunks, big-endian.
    body byte i = (i*5)&0xFF. NB the original reads LoopBeg from
    VHDR+4 (repeatHiSamples) and the loop LENGTH from VHDR+8
    (samplesPerHiCycle) -- quirk kept, so: loop 32..48, rate 16726."""
    data = bytes((i * 5) & 0xFF for i in range(SR))
    name = b'iff fixture!'              # even length: the original's
                                        # chunk walk has no pad skip
    vhdr = (struct.pack('>II', 64, 32) +        # oneShot / repeat
            struct.pack('>I', 16) +             # samplesPerHiCycle
            struct.pack('>H', 16726) +          # rate
            b'\x01\x00' + struct.pack('>I', 0x10000))
    chunks = (b'NAME' + struct.pack('>I', len(name)) + name +
              b'VHDR' + struct.pack('>I', len(vhdr)) + vhdr +
              b'BODY' + struct.pack('>I', len(data)) + data)
    open(path, 'wb').write(b'FORM' + struct.pack('>I', 4 + len(chunks)) +
                           b'8SVX' + chunks)


def gen_txw(path):
    """TX16W wave (feature 013): looped (byte 16h = 0x49), 33 kHz
    (byte 17h < 2), attack 48 + loop 32 samples, 12-bit packed pairs
    s0 = (i<<5)&0xFFF0 pattern the selftest recomputes"""
    attack, looplen = 48, 32
    n = attack + looplen                        # samples (even)
    hdr = bytearray(32)
    hdr[0:6] = b'LM8953'
    hdr[0x16] = 0x49
    hdr[0x17] = 1
    hdr[0x18:0x1B] = struct.pack('<I', attack)[0:3]
    hdr[0x1B:0x1E] = struct.pack('<I', looplen)[0:3]
    data = bytearray()
    for g in range(n // 2):
        s0 = ((2 * g) << 5) & 0xFFF0            # 12-bit <<4 values
        s1 = ((2 * g + 1) << 5) & 0xFFF0
        data.append((s0 >> 8) & 0xFF)           # b0
        data.append(((s0 & 0xF0)) | ((s1 >> 4) & 0x0F))  # b1
        data.append((s1 >> 8) & 0xFF)           # b2
    open(path, 'wb').write(bytes(hdr) + bytes(data))


def gen_its(path):
    """standalone ITS (feature 015 fixture): 8-bit signed, 1000 samples,
    C5Speed 11025, forward loop 100..900, GvL 48, Vol 40, vibrato 3/5/7.
    Values the LSS selftest checks in the Load Sample preview."""
    n = 1000
    data = bytes(((i * 5) & 0x7F) for i in range(n))
    hdr = b'IMPS' + b'FIXTURE.ITS'.ljust(12, b'\x00')
    hdr += bytes([0, 48, 0x01 | 0x10, 40])      # Zero, GvL, Flags, Vol
    hdr += b'ITS fixture'.ljust(26, b'\x00')
    hdr += bytes([0x01, 0])                     # Cvt signed, DfP off
    hdr += struct.pack('<IIIIIII', n, 100, 900, 11025, 0, 0, 80)
    hdr += bytes([3, 5, 7, 0])                  # ViS ViD ViR ViT
    assert len(hdr) == 80
    open(path, 'wb').write(hdr + data)


def gen_ls_fixture(root):
    """feature 015: a sample directory as IT's Load Sample screen sees it --
    two subdirectories, an 8-bit WAV, an ITS, a module, and a junk file"""
    import os
    os.makedirs(root + '/ACOUSTIC', exist_ok=True)
    os.makedirs(root + '/BASS', exist_ok=True)
    gen_wav8(root + '/ACOUSTIC/PIANO.WAV')
    gen_wav8(root + '/BASS/SUB.WAV')
    gen_wav8(root + '/TEST8.WAV')
    gen_its(root + '/FIXTURE.ITS')
    gen_s3m(root + '/SONG.S3M')
    open(root + '/README.TXT', 'w').write('not a sample\n')


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    gen_s3m(outdir + '/import_test.s3m')
    gen_mod(outdir + '/import_test.mod')
    gen_mtm(outdir + '/import_test.mtm')
    gen_669(outdir + '/import_test.669')
    gen_xm(outdir + '/import_test.xm')
    gen_xi(outdir + '/lib_test.xi')
    gen_ptm(outdir + '/lib_test.ptm')
    gen_far(outdir + '/lib_test.far')
    gen_krz(outdir + '/lib_test.krz')
    gen_pat(outdir + '/lib_test.pat')
    gen_wav8(outdir + '/lib_test8.wav')
    gen_wav16(outdir + '/lib_test16.wav')
    gen_wavst(outdir + '/lib_testst.wav')
    gen_wavf(outdir + '/lib_testf.wav')
    gen_wav24(outdir + '/lib_test24.wav')
    gen_iff(outdir + '/lib_test.iff')
    gen_txw(outdir + '/lib_test.txw')
    gen_ls_fixture(outdir + '/ls_fixture')
    print('wrote import_test.{s3m,mod,mtm,669,xm} + '
          'lib_test.{xi,ptm,far,krz,pat,iff,txw} + '
          'lib_test{8,16,st,f,24}.wav to', outdir)


if __name__ == '__main__':
    main()
