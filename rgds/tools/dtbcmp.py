#!/usr/bin/env python3
"""Compare two DTBs semantically, ignoring phandle numbering and node order.

A refactor from a flattened decompile to #include-based source cannot produce a
byte-identical DTB: dtc assigns phandle values in source order, so the numbers
move even when the tree is unchanged. What must be identical is the MEANING, so
this canonicalises both trees:

  - every node is addressed by its full path, and paths are sorted
  - a property cell whose value matches a phandle defined in that same tree is
    rendered as the TARGET PATH instead of the number
  - the 'phandle'/'linux,phandle' properties themselves are dropped

Caveat worth knowing: deciding which cells are phandles without bindings is a
heuristic. A plain integer that happens to equal some node's phandle is rendered
as a reference too. That is applied identically to both trees, so it is safe for
comparison, but it means "cell resolves to the same target" rather than "cell is
definitely a phandle".

Usage: dtbcmp.py a.dtb b.dtb [--dump-a out.txt] [--dump-b out.txt]
"""
import struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def parse(path):
    d = open(path, 'rb').read()
    magic, totalsize, off_struct, off_strings, off_rsvmap, ver, lastver, boot, size_strings, size_struct = \
        struct.unpack('>10I', d[:40])
    assert magic == 0xd00dfeed, f'{path}: not a DTB'
    strings = d[off_strings:off_strings + size_strings]
    pos = off_struct
    end = off_struct + size_struct
    stack, nodes, order = [], {}, []
    while pos < end:
        (tok,) = struct.unpack('>I', d[pos:pos + 4])
        pos += 4
        if tok == FDT_BEGIN_NODE:
            nul = d.index(b'\0', pos)
            name = d[pos:nul].decode('utf-8', 'replace')
            pos = (nul + 4) & ~3
            stack.append(name)
            path_ = '/' + '/'.join(x for x in stack if x)
            if path_ not in nodes:
                nodes[path_] = {}
                order.append(path_)
        elif tok == FDT_END_NODE:
            stack.pop()
        elif tok == FDT_PROP:
            plen, noff = struct.unpack('>II', d[pos:pos + 8])
            pos += 8
            val = d[pos:pos + plen]
            pos = (pos + plen + 3) & ~3
            nul = strings.index(b'\0', noff)
            pname = strings[noff:nul].decode()
            path_ = '/' + '/'.join(x for x in stack if x)
            nodes[path_][pname] = val
        elif tok in (FDT_NOP,):
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError(f'{path}: bad token {tok} at {pos}')
    return nodes, order


def canonical(nodes):
    ph2path = {}
    for path_, props in nodes.items():
        for key in ('phandle', 'linux,phandle'):
            if key in props and len(props[key]) == 4:
                ph2path[struct.unpack('>I', props[key])[0]] = path_

    PH_EXACT = {
        'clocks','interrupt-parent','interrupts-extended','operating-points-v2','power-domains',
        'memory-region','remote-endpoint','iommus','dmas','mboxes','phys','resets','cooling-device',
        'thermal-sensors','nvmem-cells','io-channels','sound-dai','next-level-cache','cpu','ports',
        'port','assigned-clocks','assigned-clock-parents','extcon','mmc-pwrseq','rockchip,grf',
        'rockchip,pmugrf','rockchip,cru','rockchip,pmu','rockchip,vo-grf','rockchip,vop-grf',
        'rockchip,php-grf','rockchip,hdmi-phy','gpio','pwms','backlight','power-supply','panel',
        'rockchip,sram','secure-regions','rockchip,drm-panel','vop-out','dsi-out','ref-clk',
    }
    # how many argument cells follow each phandle (read from the target node), so
    # clock indices that collide with a phandle number are not rendered as refs
    CELLS = {'clocks': '#clock-cells', 'assigned-clocks': '#clock-cells',
             'assigned-clock-parents': '#clock-cells', 'interrupts-extended': '#interrupt-cells',
             'dmas': '#dma-cells', 'phys': '#phy-cells', 'resets': '#reset-cells',
             'iommus': '#iommu-cells', 'io-channels': '#io-channel-cells',
             'sound-dai': '#sound-dai-cells', 'power-domains': '#power-domain-cells',
             'mboxes': '#mbox-cells', 'thermal-sensors': '#thermal-sensor-cells',
             'cooling-device': '#cooling-cells', 'gpio': '#gpio-cells', 'pwms': '#pwm-cells'}

    def is_ph_prop(name):
        return (name in PH_EXACT or name.startswith('pinctrl-') or name.endswith('-supply')
                or name.endswith('-gpios') or name.endswith('-gpio') or name == 'gpios'
                or name.endswith('-parent'))

    def ncells(ph, pname):
        cp = CELLS.get(pname)
        if not cp and (pname.endswith('-gpios') or pname.endswith('-gpio') or pname == 'gpios'):
            cp = '#gpio-cells'
        if not cp:
            return 0
        raw = nodes.get(ph2path.get(ph), {}).get(cp)
        return struct.unpack('>I', raw)[0] if raw and len(raw) == 4 else 0

    def render(val, pname=''):
        if len(val) and len(val) % 4 == 0:
            cells = struct.unpack('>%dI' % (len(val) // 4), val)
            # a phandle property is always cells; otherwise avoid misreading a
            # string list as numbers
            if is_ph_prop(pname) or not (val.endswith(b'\0') and all(32 <= c < 127 or c == 0 for c in val)):
                if is_ph_prop(pname):
                    outc, i = [], 0
                    while i < len(cells):
                        c = cells[i]
                        if c in ph2path and c != 0:
                            outc.append('&' + ph2path[c]); i += 1
                            for _ in range(ncells(c, pname)):
                                if i < len(cells):
                                    outc.append(hex(cells[i])); i += 1
                        else:
                            outc.append(hex(c)); i += 1
                    return '<' + ' '.join(outc) + '>'
                return '<' + ' '.join(hex(c) for c in cells) + '>'
        if val.endswith(b'\0') and all(32 <= c < 127 or c == 0 for c in val):
            return '"' + val[:-1].decode('utf-8', 'replace').replace('\0', '", "') + '"'
        return '[' + val.hex() + ']'

    out = []
    for path_ in sorted(nodes):
        for pname in sorted(nodes[path_]):
            if pname in ('phandle', 'linux,phandle'):
                continue
            out.append(f'{path_}  {pname} = {render(nodes[path_][pname], pname)}')
    return out


a, order_a = parse(sys.argv[1])
b, order_b = parse(sys.argv[2])
ca, cb = canonical(a), canonical(b)
for i, flag in enumerate(sys.argv):
    if flag == '--dump-a':
        open(sys.argv[i + 1], 'w').write('\n'.join(ca) + '\n')
    if flag == '--dump-b':
        open(sys.argv[i + 1], 'w').write('\n'.join(cb) + '\n')

order_mismatch = [i for i, (x, y) in enumerate(zip(order_a, order_b)) if x != y]

sa, sb = set(ca), set(cb)
only_a, only_b = sorted(sa - sb), sorted(sb - sa)
print(f'{sys.argv[1]}: {len(a)} nodes, {len(ca)} properties')
print(f'{sys.argv[2]}: {len(b)} nodes, {len(cb)} properties')
print(f'only in A: {len(only_a)}    only in B: {len(only_b)}')
if order_mismatch and set(order_a) == set(order_b):
    i = order_mismatch[0]
    print(f'NODE ORDER DIFFERS at position {i}: A has {order_a[i]}, B has {order_b[i]} '
          f'({len(order_mismatch)} positions differ)')
    print('  Node order is functional: the kernel probes platform devices in tree order and')
    print('  u-boot walks nodes in order. A reordered tree can boot to a blank screen or hang.')
if not only_a and not only_b and not order_mismatch:
    print('SEMANTICALLY IDENTICAL (same properties AND same node order)')
elif not only_a and not only_b:
    print('same properties, but NODE ORDER DIFFERS - not safe to flash')
else:
    for line in only_a[:40]:
        print('  A-only:', line[:160])
    for line in only_b[:40]:
        print('  B-only:', line[:160])
    if len(only_a) > 40 or len(only_b) > 40:
        print(f'  ... ({len(only_a)} / {len(only_b)} total)')
sys.exit(0 if not only_a and not only_b and not order_mismatch else 1)
