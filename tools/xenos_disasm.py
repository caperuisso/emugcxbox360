#!/usr/bin/env python3
"""Xenos (Xbox 360 GPU) shader microcode disassembler.

Instruction layouts follow Xenia's src/xenia/gpu/ucode.h (BSD). Used to check
the microcode emitted by src/platform/xenon/xenos_shader.* against shaders
compiled by the XDK (the libxenon cube sample).
Usage: xenos_disasm.py FILE.vsu|FILE.psu   or   xenos_disasm.py --raw FILE.bin
"""
import struct, sys

def bits(v, lo, n): return (v >> lo) & ((1 << n) - 1)

CF_OPS = ['nop','exec','exece','cexec','cexece','cexecp','cexecpe','loop_start','loop_end',
          'ccall','ret','cjmp','alloc','cexecpc','cexecpce','vfetch_done']
SC_OPS = {0:'adds',1:'adds_prev',2:'muls',3:'muls_prev',4:'muls_prev2',5:'maxs',6:'mins',7:'seqs',8:'sgts',
          9:'sges',10:'snes',11:'frcs',12:'truncs',13:'floors',14:'exp',15:'logc',16:'log',17:'rcpc',18:'rcpf',
          19:'rcp',20:'rsqc',21:'rsqf',22:'rsq',23:'maxas',24:'maxasf',25:'subs',26:'subs_prev',27:'setp_eq',
          28:'setp_ne',29:'setp_gt',30:'setp_ge',31:'setp_inv',32:'setp_pop',33:'setp_clr',34:'setp_rstr',
          35:'kills_eq',36:'kills_gt',37:'kills_ge',38:'kills_ne',39:'kills_one',40:'sqrt',42:'mulsc0',43:'mulsc1',
          44:'addsc0',45:'addsc1',46:'subsc0',47:'subsc1',48:'sin',49:'cos',50:'retain_prev'}
VEC_OPS = ['add','mul','max','min','seq','sgt','sge','sne','frc','trunc','floor','mad','cndeq','cndge','cndgt',
           'dp4','dp3','dp2add','cube','max4','setp_eq_push','setp_ne_push','setp_gt_push','setp_ge_push',
           'kill_eq','kill_gt','kill_ge','kill_ne','dst','maxa','op30','op31']
VEC_NSRC = {8:1,9:1,10:1,29:1}

def cf_unpack(d0, d1, d2):
    a = (d0, d1 & 0xFFFF)
    b = ((d1 >> 16) | ((d2 << 16) & 0xFFFFFFFF), d2 >> 16)
    return [a, b]

def cf_str(w0, w1):
    op = bits(w1, 12, 4)
    name = CF_OPS[op]
    if op in (1, 2, 3, 4, 5, 6, 13, 14):
        addr, cnt, seq = bits(w0, 0, 12), bits(w0, 12, 3), bits(w0, 16, 12)
        extra = ''
        if op in (3, 4): extra = ' b%d==%d' % (bits(w1, 2, 8), bits(w1, 10, 1))
        if op in (5, 6, 13, 14): extra = ' p==%d' % bits(w1, 10, 1)
        return '%s addr=%d cnt=%d seq=%s%s' % (name, addr, cnt, format(seq, '012b'), extra), (addr, cnt, seq)
    if op == 12:
        return 'alloc %s size=%d' % (['none', 'position', 'interp/colors', 'memory'][bits(w1, 9, 2)], bits(w0, 0, 3)), None
    return name + ' %08x %04x' % (w0, w1), None

def swz_rel(s, n=4):
    return ''.join('xyzw'[((s >> (2 * i)) + i) & 3] for i in range(n))

def fetch_str(w):
    op = bits(w[0], 0, 5)
    src, dst = bits(w[0], 5, 6), bits(w[0], 12, 6)
    dsw = ''.join('xyzw01?_'[bits(w[1], 3 * i, 3)] for i in range(4))
    if op == 0:
        ci = bits(w[0], 20, 5) * 3 + bits(w[0], 25, 2)
        return 'vfetch r%d.%s, r%d.%s, vf%d fmt=%d stride=%d off=%d signed=%d unnorm=%d mini=%d prefetch=%d' % (
            dst, dsw, src, 'xyzw'[bits(w[0], 30, 2)], ci, bits(w[1], 16, 6), bits(w[2], 0, 8),
            bits(w[2], 8, 23), bits(w[1], 12, 1), bits(w[1], 13, 1), bits(w[1], 30, 1), bits(w[0], 27, 3))
    ssw = ''.join('xyzw'[bits(w[0], 26 + 2 * i, 2)] for i in range(3))
    return 'tfetch r%d.%s, r%d.%s, tf%d mag=%d min=%d mip=%d dim=%d comp_lod=%d valid_only=%d' % (
        dst, dsw, src, ssw, bits(w[0], 20, 5), bits(w[1], 12, 2), bits(w[1], 14, 2), bits(w[1], 16, 2),
        bits(w[2], 14, 2), bits(w[1], 28, 1), bits(w[0], 19, 1))

def alu_str(w):
    vdst, sdst = bits(w[0], 0, 6), bits(w[0], 8, 6)
    export = bits(w[0], 15, 1)
    vmask, smask = bits(w[0], 16, 4), bits(w[0], 20, 4)
    sop = bits(w[0], 26, 6)
    vop = bits(w[2], 24, 5)
    def src(i):
        reg = bits(w[2], 16 - 8 * (i - 1), 8) if False else [None, bits(w[2], 16, 8), bits(w[2], 8, 8), bits(w[2], 0, 8)][i]
        sel = [None, bits(w[2], 31, 1), bits(w[2], 30, 1), bits(w[2], 29, 1)][i]
        swz = [None, bits(w[1], 16, 8), bits(w[1], 8, 8), bits(w[1], 0, 8)][i]
        neg = [None, bits(w[1], 26, 1), bits(w[1], 25, 1), bits(w[1], 24, 1)][i]
        r = ('r%d' % (reg & 0x3F)) if sel else ('c%d' % reg)
        if sel and reg & 0x80: r = '|%s|' % r
        return ('-' if neg else '') + r + '.' + swz_rel(swz)
    m = lambda k: ''.join('xyzw'[i] if k & (1 << i) else '_' for i in range(4))
    dname = lambda d: (('oC%d' % d if d < 4 else 'oDepth' if d == 61 else 'o%d' % d) if d != 62 else 'oPos') if export else 'r%d' % d
    out = []
    n = VEC_NSRC.get(vop, 3 if vop in (11, 12, 13, 14, 17) else 2)
    if vmask or not smask:
        out.append('%s %s.%s, %s' % (VEC_OPS[vop], dname(vdst), m(vmask), ', '.join(src(i) for i in range(1, n + 1))))
    if smask or sop != 50:
        sd = dname(vdst) if export else 'r%d' % sdst
        out.append('+ %s %s.%s, %s' % (SC_OPS.get(sop, 'sop%d' % sop), sd, m(smask), src(3)))
    if export: out.append('(export)')
    return ' '.join(out)

def disasm(words, label=''):
    # Control flow: pairs of 48-bit instructions until an ending exec
    cfs = []
    i = 0
    end = None
    while i + 3 <= len(words):
        for w0, w1 in cf_unpack(*words[i:i + 3]):
            cfs.append((w0, w1))
        i += 3
        if any(bits(w1, 12, 4) in (2, 4, 6, 14) for w0, w1 in cfs[-2:]): break
    for n, (w0, w1) in enumerate(cfs):
        s, ex = cf_str(w0, w1)
        print('  cf%-2d %s' % (n, s))
        if ex:
            addr, cnt, seq = ex
            for k in range(cnt):
                ins = words[(addr + k) * 3:(addr + k) * 3 + 3]
                is_fetch = (seq >> (2 * k)) & 1
                print('        %3d %s %s' % (addr + k, ' '.join('%08x' % x for x in ins),
                                           fetch_str(ins) if is_fetch else alu_str(ins)))

def load_container(path):
    d = open(path, 'rb').read()
    magic, offset = struct.unpack('>II', d[:8])
    off_shader = struct.unpack('>I', d[0x18:0x1c])[0]
    sh_off, sh_size, pc, cm = struct.unpack('>IIII', d[off_shader:off_shader + 16])
    code = d[offset + sh_off: offset + sh_off + sh_size]
    print('%s: magic=%08x program_control=%08x context_misc=%08x size=%d' % (path, magic, pc, cm, sh_size))
    return list(struct.unpack('>%dI' % (len(code) // 4), code))

if __name__ == '__main__':
    if sys.argv[1] == '--raw':
        d = open(sys.argv[2], 'rb').read()
        disasm(list(struct.unpack('>%dI' % (len(d) // 4), d)))
    else:
        disasm(load_container(sys.argv[1]))
