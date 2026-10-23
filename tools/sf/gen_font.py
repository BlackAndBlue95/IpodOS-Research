#!/usr/bin/env python3
"""Modern-style font: Apple's SF Compact (from this Mac) cut down to stand in for the iPod's Helvetica.

    gen_font.py HELVETICA.ttf HELVETICABOLD.ttf OUTDIR     (needs fontTools; run from a venv)

For each weight: an instance of the variable font (wght 400 / 700), only the characters Helvetica
maps, no layout or variation tables, Helvetica's names (the OS picks fonts by family and style) and
Helvetica's line metrics (layouts assume them), padded to the original's size so either can stand in
for the other. SF's GPOS pair kerning for Latin-1 and punctuation goes into a version 0 'kern' table
(the one FreeType reads; region E's kern.c applies it). The output is Apple's font: OUTDIR is
gitignored and must not be committed."""
import sys
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
from fontTools.varLib import instancer
from fontTools import subset
from fontTools.ttLib.tables._c_m_a_p import cmap_format_4, cmap_format_6
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen

SRC = '/System/Library/Fonts/SFCompact.ttf'
KERN_MIN = 20     # font units (2048 per em): 0.17 px at 17 px


def kern_pairs(f):
    """GPOS 'kern' pair adjustments between Latin-1 and punctuation characters, as {(u1, u2): units}"""
    cmap = f.getBestCmap()
    chars = [u for u in cmap if 0x20 <= u < 0x7f or 0xa0 <= u < 0x100 or 0x2010 <= u <= 0x203a]
    glyphs = {cmap[u] for u in chars}
    gpos = f['GPOS'].table
    total = {}
    for li in sorted({i for fr in gpos.FeatureList.FeatureRecord if fr.FeatureTag == 'kern'
                      for i in fr.Feature.LookupListIndex}):
        lookup, got = gpos.LookupList.Lookup[li], {}
        assert lookup.LookupType in (2, 9), lookup.LookupType
        for st in lookup.SubTable:            # within a lookup the first subtable that covers a pair wins
            if lookup.LookupType == 9: st = st.ExtSubTable
            if st.Format == 1:
                for i, g1 in enumerate(st.Coverage.glyphs):
                    if g1 not in glyphs: continue
                    for r in st.PairSet[i].PairValueRecord:
                        if r.SecondGlyph in glyphs: got.setdefault((g1, r.SecondGlyph), getattr(r.Value1, 'XAdvance', 0) or 0)
            else:
                c1, c2 = st.ClassDef1.classDefs, st.ClassDef2.classDefs
                for g1 in glyphs & set(st.Coverage.glyphs):
                    row = st.Class1Record[c1.get(g1, 0)].Class2Record
                    for g2 in glyphs: got.setdefault((g1, g2), getattr(row[c2.get(g2, 0)].Value1, 'XAdvance', 0) or 0)
        for k, v in got.items(): total[k] = total.get(k, 0) + v   # lookups add up
    return {(u1, u2): total[cmap[u1], cmap[u2]] for u1 in chars for u2 in chars
            if abs(total.get((cmap[u1], cmap[u2]), 0)) >= KERN_MIN}


def make(orig_path, weight, out_path):
    orig = TTFont(orig_path)
    uni = orig.getBestCmap()
    f = instancer.instantiateVariableFont(TTFont(SRC), {'wght': weight, 'opsz': 19, 'GRAD': 400})
    pairs = kern_pairs(f)
    opts = subset.Options()
    opts.layout_features = []
    opts.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'MERG', 'meta', 'trak', 'STAT', 'HVAR', 'MVAR', 'DSIG']
    opts.name_IDs = ['*']
    opts.notdef_outline = True
    opts.glyph_names = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=list(uni))
    sub.subset(f)
    # names: exactly Helvetica's, on both platforms
    f['name'].names = []
    for rec in orig['name'].names:
        if rec.nameID > 6 or rec.platformID != 1 or rec.langID != 0: continue
        s = rec.toUnicode(errors='replace')
        f['name'].setName(s, rec.nameID, 1, 0, 0)
        f['name'].setName(s, rec.nameID, 3, 1, 0x409)
    # character maps like the original's: Unicode BMP (0,3 and 3,1) and Mac Roman
    best = f.getBestCmap()
    tables = []
    for pid, eid in ((0, 3), (3, 1)):
        t = cmap_format_4(4); t.platformID, t.platEncID, t.language = pid, eid, 0
        t.cmap = {c: g for c, g in best.items() if c <= 0xffff}; tables.append(t)
    mac = cmap_format_6(6); mac.platformID, mac.platEncID, mac.language = 1, 0, 0
    order = f.getGlyphOrder()
    mac.cmap = {}
    for code in range(256):
        try: u = ord(bytes([code]).decode('mac_roman'))
        except UnicodeDecodeError: continue
        if u in best: mac.cmap[code] = best[u]
    mac.cmap = {c: mac.cmap.get(c, order[0]) for c in range(256)}
    tables.append(mac)
    f['cmap'].tables = tables
    # Helvetica's vertical metrics, so text sits where the layouts expect it
    for k in ('ascent', 'descent', 'lineGap'):
        setattr(f['hhea'], k, getattr(orig['hhea'], k))
    f['head'].macStyle = orig['head'].macStyle
    f['post'].formatType = 3.0
    # one format 0 subtable: FreeType 2.3 binary-searches it, and its 16-bit length holds 10920 pairs
    kt = KernTable_format_0()
    kt.version, kt.coverage, kt.format, kt.apple = 0, 1, 0, False
    kt.kernTable = {(best[u1], best[u2]): v for (u1, u2), v in pairs.items() if u1 in best and u2 in best}
    assert len(kt.kernTable) <= 10920, len(kt.kernTable)
    f['kern'] = newTable('kern'); f['kern'].version, f['kern'].kernTables = 0, [kt]
    # Apple's iPod font engine hung on the plain instance: it wants the hinting limits in maxp that
    # Helvetica has (SF has none) and no OS/2 table. Composites are flattened too (SF nests them
    # three deep, Helvetica one), so nothing depends on the engine's component handling.
    if 'OS/2' in f: del f['OS/2']
    gs, glyf = f.getGlyphSet(), f['glyf']
    flat = {}
    for g in f.getGlyphOrder():
        if glyf[g].isComposite():
            rp = DecomposingRecordingPen(gs); gs[g].draw(rp)
            tp = TTGlyphPen(None); rp.replay(tp); flat[g] = tp.glyph()
    for g, v in flat.items(): glyf[g] = v; v.recalcBounds(glyf)
    m, om = f['maxp'], orig['maxp']
    for k in ('maxZones', 'maxTwilightPoints', 'maxStorage', 'maxFunctionDefs', 'maxInstructionDefs',
              'maxStackElements', 'maxSizeOfInstructions'):
        setattr(m, k, getattr(om, k))
    m.maxPoints = max(len(glyf[g].coordinates) for g in f.getGlyphOrder() if glyf[g].numberOfContours > 0)
    m.maxContours = max(glyf[g].numberOfContours for g in f.getGlyphOrder())
    m.maxCompositePoints = m.maxCompositeContours = m.maxComponentElements = m.maxComponentDepth = 0
    f.save(out_path)
    data = open(out_path, 'rb').read()
    size = len(open(orig_path, 'rb').read())
    if len(data) < size:
        open(out_path, 'wb').write(data + b'\0' * (size - len(data)))
    print('%s: %d glyphs, %d kerning pairs, %d bytes (original %d)' % (out_path, f['maxp'].numGlyphs, len(kt.kernTable), len(data), size))


make(sys.argv[1], 400, sys.argv[3] + '/Helvetica.ttf')
make(sys.argv[2], 700, sys.argv[3] + '/HelveticaBold.ttf')
