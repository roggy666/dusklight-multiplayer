import re, bisect, sys

MAP = r'C:\OGames\Dolphin\dusklight\build\windows-msvc-relwithdebinfo\dusklight.map'
LOG = sys.argv[1]
BASE = 0x140000000
BS = chr(92)  # backslash

pat = re.compile(r'^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+')
syms = []
inpub = False
for line in open(MAP, encoding='utf-8', errors='replace'):
    if 'Publics by Value' in line:
        inpub = True
        continue
    if inpub:
        m = pat.match(line)
        if m:
            syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
addrs = [s[0] for s in syms]

def resolve(rva):
    t = BASE + rva
    i = bisect.bisect_right(addrs, t) - 1
    if i < 0:
        return '???'
    return '%s +0x%x' % (syms[i][1], t - syms[i][0])

lines = open(LOG, encoding='utf-8', errors='replace').read().splitlines()
frame_re = re.compile(r'^#(\d+) abs=0x([0-9a-f]+).*?rva=0x([0-9a-f]+) module="([^"]+)"')
thr_re = re.compile(r'^--- Thread (\d+)(.*?) ---')
started = False
cur = 'MAIN (hung)'
out = []
for ln in lines:
    if 'Backtrace:' in ln:
        started = True
        out.append(('HDR', cur))
        continue
    m = thr_re.match(ln)
    if m:
        cur = 'Thread %s%s' % (m.group(1), m.group(2))
        out.append(('HDR', cur))
        continue
    if started:
        fm = frame_re.match(ln)
        if fm:
            idx, absa, rva, mod = fm.groups()
            modshort = mod.split(BS)[-1]
            if 'dusklight.exe' in mod:
                out.append(('F', '  #%s %s' % (idx, resolve(int(rva, 16)))))
            else:
                out.append(('F', '  #%s [%s +0x%s]' % (idx, modshort, rva)))

for kind, txt in out:
    if kind == 'HDR':
        print('\n=== ' + txt + ' ===')
    else:
        print(txt)
