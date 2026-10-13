#!/usr/bin/env python3
"""Build an offline signal-callback fixture from the exact device vendor ELF.

The fixture retains its instruction layout and relocations. Unrelated private
vendor callbacks are replaced by no-op stubs; it registers no Android hooks.
"""
import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED = '574d81868eec62ca5a83368887d4b4ad6665404586147099224980cf442c7792'

def prepare(source, destination):
    data = Path(source).read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError('vendor ELF does not match the recorded PLZ110 module')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    ents, count, strings = struct.unpack_from('<HHH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff+i*ents) for i in range(count)]
    def blob(section):
        return data[section[4]:section[4]+section[5]]
    def string(text, offset):
        return text[offset:text.index(0, offset)].decode()
    names = blob(sections[strings])
    indexes = {string(names, s[0]):i for i,s in enumerate(sections)}
    symtab = sections[indexes['.symtab']]
    names = blob(sections[symtab[6]])
    symbols = []
    for offset in range(symtab[4], symtab[4]+symtab[5], symtab[9]):
        name, _, _, section, value, size = struct.unpack_from('<IBBHQQ', data, offset)
        symbols.append((string(names,name), section, value, size))
    _, section, start, size = next(s for s in symbols if s[0] == 'send_signal_catcher')
    words = struct.unpack('<%dI' % (size//4), blob(sections[section])[start:start+size])
    relocations = {}
    for s in sections:
        if s[1] != 4 or s[7] != section:
            continue
        for offset in range(s[4], s[4]+s[5], s[9]):
            where, info, addend = struct.unpack_from('<QQq', data, offset)
            if start <= where < start+size:
                relocations[(where-start)//4] = (info & 0xffffffff, symbols[info >> 32], addend)
    lines = ['.text', '.balign 4', '.global send_signal_catcher',
             '.type send_signal_catcher, %function', 'send_signal_catcher:']
    for index, word in enumerate(words):
        if index not in relocations:
            lines.append('.inst 0x%08x' % word)
            continue
        kind, symbol, addend = relocations[index]
        name, si, _, _ = symbol
        if kind == 283:
            # Both private vendor routines are outside the ownership path.
            if si == section or name == 'check_system_server_hang':
                name = 'fixture_noop'
            lines.append('bl ' + name)
        elif kind == 275:
            lines.append('adrp x%d, fixture_rodata+%d' % (word & 31,addend))
        elif kind == 277:
            lines.append('add x%d, x%d, :lo12:fixture_rodata+%d' % (word & 31,(word >> 5)&31,addend))
        else:
            raise ValueError('unexpected relocation')
    lines += ['.size send_signal_catcher, .-send_signal_catcher',
              'fixture_noop:', 'mov w0, #0', 'ret', '.section .rodata', '.balign 8', 'fixture_rodata:']
    rodata = blob(sections[indexes['.rodata']])
    for offset in range(0,len(rodata),16):
        lines.append('.byte ' + ','.join(str(n) for n in rodata[offset:offset+16]))
    dest = Path(destination)
    dest.mkdir(parents=True,exist_ok=True)
    (dest/'signal-fixture.S').write_text('\n'.join(lines)+'\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vendor_module')
    parser.add_argument('destination')
    args = parser.parse_args()
    prepare(args.vendor_module,args.destination)
