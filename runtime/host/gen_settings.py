#!/usr/bin/env python3
"""Writes settings_data.h and settings_data.json: our rows and screens in Apple's Settings menu.
Records are cloned from the OS's own resources, so every other byte stays Apple's.
  gen_settings.py OSOS.dec OUT.h OUT.json"""
import json, struct, sys
from uires import Package, DATA, BASE, PKG

pkg = Package(sys.argv[1])
D = pkg.D

SCREEN, SCREEN_LAYOUT = 0x0dad0c2e, 0x0dad0c43           # the main menu pushes Settings with About's layout
BACKLIGHT_SCREEN, BACKLIGHT_LAYOUT, BACKLIGHT_ROW = 0x0dad0c5e, 0x0dad0c66, 0x0dad0c35
RADIO_LAYOUT = 0x0dad0c6b
STATUSBAR_TEXT, LIST_TEMPLATE = 0x0dad0172, 0x0dad017f

# ids we add; each is above Apple's largest id of its type
THEME_SCREEN, THEME_LAYOUT, ACCENT_SCREEN, ACCENT_LAYOUT = 0x0dad0f80, 0x0dad0f81, 0x0dad0f82, 0x0dad0f83
LIBRARY_SCREEN, LIBRARY_LAYOUT = 0x0dad0f88, 0x0dad0f89
PREVIEW_LAYOUT = (0x0dad0f84, 0x0dad0f85, 0x0dad0f8a)     # Settings-screen layouts shown while our rows are highlighted
PREVIEW_VLYT = (0x0dad0f86, 0x0dad0f87, 0x0dad0f8b)       # the info pane of each (shared by the submenu canvases)
ROW_PID = (0x0dad0f60, 0x0dad0f61, 0x0dad0f62)            # our rows in Settings
ITEM_ID = (0x54, 0x55, 0x56)                              # submenu lists
CANVAS = (0x448, 0x449, 0x44a, 0x44b, 0x45c, 0x45d)       # theme, accent, library submenus; theme, accent, library previews
INST0 = 0x44c                                             # 4 sub-instances per canvas: status bar, background, info, list (0x44c..0x45b, 0x45e..)
KEY_TEXT = (0x8930, 0x8931, 0x8932)                       # provider keys: current value text
KEY_CHECK = (0x8940, 0x8950, 0x8960)                      # provider keys: checkmark image, + option index
THEME_OPTIONS = ['Light', 'Dark', 'Automatic', 'Modern style', 'Restart to apply']
ACCENT_OPTIONS = ['Blue', 'Purple', 'Pink', 'Red', 'Orange', 'Yellow', 'Green', 'Graphite']
LIBRARY_OPTIONS = ['Sync library now', 'Power state to log']
STR_LABEL = (0x0dad0f70, 0x0dad0f71, 0x0dad0f8c)
STR_THEME0, STR_ACCENT0, STR_LIBRARY0 = 0x0dad0f72, 0x0dad0f78, 0x0dad0f8d   # 5 theme options (0f72..0f76), 8 accents (0f78..0f7f), 1 library (0f8d)
STR0 = (STR_THEME0, STR_ACCENT0, STR_LIBRARY0)
GROUPS = [('Theme', THEME_OPTIONS, THEME_SCREEN, THEME_LAYOUT, 'SetTheme_'), ('Accent Colour', ACCENT_OPTIONS, ACCENT_SCREEN, ACCENT_LAYOUT, 'SetAccent_'),
          ('Library', LIBRARY_OPTIONS, LIBRARY_SCREEN, LIBRARY_LAYOUT, 'SetLibrary_')]
NG = len(GROUPS)
NAMES = [('E_Theme_Screen', THEME_SCREEN), ('E_Theme_Layout', THEME_LAYOUT), ('E_Accent_Screen', ACCENT_SCREEN), ('E_Accent_Layout', ACCENT_LAYOUT),
         ('E_Library_Screen', LIBRARY_SCREEN), ('E_Library_Layout', LIBRARY_LAYOUT),
         ('E_Settings_Theme_Layout', PREVIEW_LAYOUT[0]), ('E_Settings_Accent_Layout', PREVIEW_LAYOUT[1]), ('E_Settings_Library_Layout', PREVIEW_LAYOUT[2])]
NAMES_TABLE, NAMES_COUNT, NAMES_NEW_COUNT = 0x083f8728, 3920, 3936   # 3936 = 0xf60, an ARM immediate

def cstr(a):
    o = a - BASE
    return D[o:D.index(b'\0', o)].decode('latin1')
byname = {}
for k in range(NAMES_COUNT):
    p, pid = struct.unpack_from('<II', D, NAMES_TABLE - BASE + 8 * k)
    byname[cstr(p)] = pid
allres = {}
for t in pkg.types:
    for p, o, s in pkg.index(t):
        allres.setdefault(p, {})[t] = D[DATA + o - BASE:DATA + o - BASE + s]
def res(t, pid): return allres[pid][t]
def w32(b, off): return struct.unpack_from('<I', b, off)[0]
def patched(b, off, v):
    b = bytearray(b); struct.pack_into('<I', b, off, v); return bytes(b)

new = {}            # (type, id) -> bytes, appended to the index
inplace = {}        # (type, id) -> bytes, replacing Apple's entry's data
def add(t, pid, data):
    assert (t, pid) not in new
    assert pid > max(p for p, o, s in pkg.index(t)), (t, pid)
    new[(t, pid)] = bytes(data)

# SORC: {count, {0xc, tag, class, id, role}}; class 0x80 = resource id, 0x8900 = provider key
tag = res('SORC', 0x760)[8:12]
def sorc(*entries):
    return struct.pack('<I', len(entries)) + b''.join(struct.pack('<I4sIII', 0xc, tag, cls, rid, role) for cls, rid, role in entries)
assert sorc((0x80, 0x0dad0b42, 1), (0x8900, 0x8904, 0xa)) == res('SORC', 0x760)
next_sorc = [0x8a0]
def new_sorc(*entries):
    sid = next_sorc[0]; next_sorc[0] += 1
    add('SORC', sid, sorc(*entries)); return sid

# ---- Str ----
def new_str(pid, text): add('Str ', pid, text.encode() + b'\0')
for k, (label, *_ ) in enumerate(GROUPS): new_str(STR_LABEL[k], label)
for k, s in enumerate(THEME_OPTIONS): new_str(STR_THEME0 + k, s)
for k, s in enumerate(ACCENT_OPTIONS): new_str(STR_ACCENT0 + k, s)
for k, s in enumerate(LIBRARY_OPTIONS): new_str(STR_LIBRARY0 + k, s)

# ITEM: {count, {size 0x98, 0, payload}}; payload +0x30 pid, +0x5c index, +0x60 kind, +0x68 SORC
def item_rows(item):
    n = w32(item, 0); p, rows = 4, []
    for i in range(n):
        sz = w32(item, p); rows.append(item[p:p + 8 + sz]); p += 8 + sz
    assert p == len(item)
    return rows
def item_build(rows): return struct.pack('<I', len(rows)) + b''.join(rows)
def row_clone(row, pid, index, sorc_id):
    r = bytearray(row)
    struct.pack_into('<I', r, 8 + 0x30, pid)
    struct.pack_into('<I', r, 8 + 0x5c, (w32(r, 8 + 0x5c) & 0xffff0000) | index)
    struct.pack_into('<I', r, 8 + 0x68, sorc_id)
    return bytes(r)
main_rows = item_rows(res('ITEM', 0x41))
backlight_main_row = [r for r in main_rows if w32(r, 8 + 0x30) == BACKLIGHT_ROW][0]
assert w32(backlight_main_row, 8 + 0x60) == 5                      # kind 5: opens a submenu
label_sorc = [new_sorc((0x80, STR_LABEL[k], 1)) for k in range(NG)]
main_rows += [row_clone(backlight_main_row, ROW_PID[k], len(main_rows) + k, label_sorc[k]) for k in range(NG)]
inplace[('ITEM', 0x41)] = item_build(main_rows)
sub_row = item_rows(res('ITEM', 0x44))[0]                            # Backlight submenu row, kind 2
assert w32(sub_row, 8 + 0x60) == 2
option_pids = []
for g, (label, options, scr, lay, action) in enumerate(GROUPS):
    rows, pids = [], []
    for k, text in enumerate(options):
        pid = 0x0dad0f90 + 8 * g + k
        sid = new_sorc((0x80, STR0[g] + k, 1), (0x8900, KEY_CHECK[g] + k, 0xc))
        rows.append(row_clone(sub_row, pid, k, sid)); pids.append(pid)
    add('ITEM', ITEM_ID[g], item_build(rows)); option_pids.append(pids)

# CEVT: {count, {str name, u8 flag, str action, {count, {str cmd, {count, str arg}}}}}
def cevt_parse(b):
    p = 0
    def rs():
        nonlocal p
        n = w32(b, p); p += 4; s = b[p:p + n]; p += n; return s
    cnt = w32(b, p); p += 4; events = []
    for i in range(cnt):
        name = rs(); flag = b[p]; p += 1; action = rs(); ncmd = w32(b, p); p += 4
        cmds = []
        for k in range(ncmd):
            cmd = rs(); argc = w32(b, p); p += 4; cmds.append((cmd, [rs() for _ in range(argc)]))
        events.append((name, flag, action, cmds))
    assert p == len(b)
    return events
def ws(s): return struct.pack('<I', len(s)) + s
def cevt_build(events):
    out = struct.pack('<I', len(events))
    for name, flag, action, cmds in events:
        out += ws(name) + bytes([flag]) + ws(action) + struct.pack('<I', len(cmds))
        for cmd, args in cmds:
            out += ws(cmd) + struct.pack('<I', len(args)) + b''.join(ws(a) for a in args)
    return out
main_events = cevt_parse(res('CEVT', SCREEN))
assert cevt_build(main_events) == res('CEVT', SCREEN)
for g, (label, options, scr, lay, action) in enumerate(GROUPS):
    main_events.append((b'list.pid.%d.chosen' % ROW_PID[g], 0x31, b'', [(b'navigator.PushScreen', [NAMES[2 * g][0].encode(), NAMES[2 * g + 1][0].encode()])]))
    main_events.append((b'list.pid.%d.delayedselected' % ROW_PID[g], 0x31, b'', [(b'navigator.SwitchLayout', [NAMES[2 * NG + g][0].encode()])]))
    add('CEVT', scr, cevt_build([(b'list.pid.%d.chosen' % pid, 0x31, b'%s%d' % (action.encode(), k), []) for k, pid in enumerate(option_pids[g])]))
inplace[('CEVT', SCREEN)] = cevt_build(main_events)

# ---- screens: SCST controller class, SLst layouts, VSlt list -> ITEM ----
scst = res('SCST', SCREEN)
assert scst == b'TSilverSettingsMenuListCntlr'
slst_entry = res('SLst', BACKLIGHT_SCREEN)
assert slst_entry[:4] == b'\1\0\0\0' and w32(slst_entry, 12) == BACKLIGHT_LAYOUT and len(slst_entry) == 24
vslt = res('VSlt', BACKLIGHT_SCREEN)
assert w32(vslt, 12) == LIST_TEMPLATE and w32(vslt, 16) == 0x44 and len(vslt) == 20
for g, (label, options, scr, lay, action) in enumerate(GROUPS):
    add('SCST', scr, scst)
    add('SLst', scr, patched(slst_entry, 12, lay))
    add('VSlt', scr, patched(vslt, 16, ITEM_ID[g]))
main_slst = res('SLst', SCREEN)
assert w32(main_slst, 0) == 19 and len(main_slst) == 4 + 19 * 20
inplace[('SLst', SCREEN)] = patched(main_slst, 0, 19 + NG) + b''.join(patched(slst_entry[4:], 8, PREVIEW_LAYOUT[g]) for g in range(NG))

# layouts: SEVT (no events), SLyt -> canvas; canvases: VCrv, SSin of 4 sub-instances
slyt = res('SLyt', BACKLIGHT_LAYOUT)
assert w32(slyt, 12) == 0x19c and len(slyt) == 20
radio_canvas = w32(res('SLyt', RADIO_LAYOUT), 12)
ssin = res('SSin', radio_canvas)
vcrv = res('VCrv', radio_canvas)
assert w32(ssin, 0) == 4 and len(ssin) == 4 + 4 * 28 and len(vcrv) == 60
sub = [struct.unpack_from('<7I', ssin, 4 + 28 * k) for k in range(4)]
assert [byname['StatusBarWhite_Template'], byname['SettingsBgnd_Template'], byname['SettingsInfo_Template'], byname['MainMenu_Template']] == [s[3] for s in sub]
statusbar_vcvs = res('VCvs', sub[0][2])
assert w32(statusbar_vcvs, 8) == STATUSBAR_TEXT and len(statusbar_vcvs) == 28
settings_title_sorc = w32(res('VCvs', 0x373), 24)                   # the Settings layouts' status-bar title
assert res('Str ', w32(res('SORC', settings_title_sorc), 16)) == b'Settings\0'
title_sorc = [new_sorc((0x80, STR_LABEL[g], 1)) for g in range(NG)]
layouts = [(THEME_LAYOUT, CANVAS[0], title_sorc[0], 0), (ACCENT_LAYOUT, CANVAS[1], title_sorc[1], 1),
           (PREVIEW_LAYOUT[0], CANVAS[2], settings_title_sorc, 0), (PREVIEW_LAYOUT[1], CANVAS[3], settings_title_sorc, 1),
           (LIBRARY_LAYOUT, CANVAS[4], title_sorc[2], 2), (PREVIEW_LAYOUT[2], CANVAS[5], settings_title_sorc, 2)]
INST = [INST0 + 4 * n for n in range(4)] + [0x45e, 0x462]   # sub-instance ids per canvas
for n, (lay, canvas, title, g) in enumerate(layouts):
    add('SEVT', lay, b'\0\0\0\0')
    add('SLyt', lay, patched(slyt, 12, canvas))
    add('VCrv', canvas, vcrv)
    s = bytearray(ssin)
    for k in range(4):
        inst = INST[n] + k
        struct.pack_into('<I', s, 4 + 28 * k + 8, inst)
        if k == 2: struct.pack_into('<I', s, 4 + 28 * k + 16, PREVIEW_VLYT[g])
        add('VCrv', inst, vcrv)
        if k == 0: add('VCvs', inst, patched(statusbar_vcvs, 24, title))
    add('SSin', canvas, bytes(s))

# info pane: title, icon, detail (the current value, from our provider)
MAINMENU_PREVIEW = byname['SettingsInfo_Template_MainMenu_Layout']
vlyt = res('VLyt', MAINMENU_PREVIEW)
assert res('TEVT', MAINMENU_PREVIEW) == b'\0\0\0\0'
cors = [i + 4 for i in range(0, len(vlyt) - 4, 4) if w32(vlyt, i) == 0x534f5243]   # the SORC id follows each 'SORC' tag
assert len(cors) == 3, cors
ICONS = (byname['Settings_Brightness_Image'], byname['Settings_MainMenu_Image'], byname['Settings_MainMenu_Image'])
for g in range(NG):
    v = bytearray(vlyt)
    for off, sid in zip(cors, (new_sorc((0x80, STR_LABEL[g], 1)), new_sorc((0x80, ICONS[g], 1)), new_sorc((0x8900, KEY_TEXT[g], 1)))):
        struct.pack_into('<I', v, off, sid)
    add('VLyt', PREVIEW_VLYT[g], bytes(v))
    add('TEVT', PREVIEW_VLYT[g], b'\0\0\0\0')

# index: per type, Apple's table plus our entries; mkpatch18.py fills in the offsets
def type_record_addr(t): return PKG + 12 + 16 * list(pkg.types).index(t)
def entry_addr(t, pid):
    cnt, dense, ioff = pkg.types[t]
    for k in range(cnt):
        if w32(D, PKG - BASE + ioff + 12 * k) == pid: return PKG + ioff + 12 * k
    raise KeyError((t, pid))
def csym(t, pid): return 'settings_r_%s_%x' % (t.strip().lower(), pid)
types = {}
for (t, pid), data in sorted(new.items()):
    types.setdefault(t, []).append((pid, data))
plan = {'types': {}, 'names': {}}
tables = {}
for t in sorted(set(types) | set(t for t, pid in inplace)):
    entries = types.get(t, [])
    cnt, dense, ioff = pkg.types[t]
    idx = bytearray(D[PKG - BASE + ioff:PKG - BASE + ioff + 12 * cnt])
    if dense: assert [p for p, d in entries] == list(range(cnt + 1, cnt + 1 + len(entries))), t
    # replaced records: the copy gets a placeholder offset and the new size; the OS reads only the copy
    replaced = []
    for (tt, pid), data in inplace.items():
        if tt != t: continue
        pos = (entry_addr(t, pid) - PKG - ioff) // 12
        assert struct.unpack_from('<I', idx, 12 * pos)[0] == pid
        struct.pack_into('<II', idx, 12 * pos + 4, 0, len(data))
        replaced.append({'id': pid, 'pos': pos, 'sym': csym(t, pid), 'size': len(data)})
    tables[t] = bytes(idx) + b''.join(struct.pack('<III', pid, 0, len(data)) for pid, data in entries)
    plan['types'][t] = {'typerec': type_record_addr(t), 'count': cnt, 'orig_index': ioff, 'tab': 'settings_index_%s' % t.strip().lower(),
                        'new': [{'id': pid, 'sym': csym(t, pid), 'size': len(data)} for pid, data in entries], 'replaced': replaced}
plan['names'] = {'sym': 'settings_names', 'table': NAMES_TABLE, 'count': NAMES_COUNT, 'new_count': NAMES_NEW_COUNT,
                 'literals': [0x082856bc, 0x082855f0], 'cmps': [[0x0828564c, 0xe3540ef5, 0xe3540ef6], [0x082855e0, 0xe3510ef5, 0xe3510ef6]]}
assert len(NAMES) <= NAMES_NEW_COUNT - NAMES_COUNT

def carr(name, data, const='const uint8_t'):
    s = '%s __attribute__((section(".rodata.keep"), aligned(4))) %s[%d] = {\n' % (const, name, len(data))
    for i in range(0, len(data), 16):
        s += '    ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',\n'
    return s + '};\n'
with open(sys.argv[2], 'w') as f:
    f.write('/* generated by host/gen_settings.py from the OS Settings resources; do not edit */\n')
    f.write('#define SETTINGS_SCREEN 0x%08x\n#define SETTINGS_LAYOUT 0x%08x\n' % (SCREEN, SCREEN_LAYOUT))
    f.write('#define THEME_SCREEN 0x%08x\n#define THEME_LAYOUT 0x%08x\n#define ACCENT_SCREEN 0x%08x\n#define ACCENT_LAYOUT 0x%08x\n' % (THEME_SCREEN, THEME_LAYOUT, ACCENT_SCREEN, ACCENT_LAYOUT))
    f.write('#define LIBRARY_SCREEN 0x%08x\n#define LIBRARY_LAYOUT 0x%08x\n' % (LIBRARY_SCREEN, LIBRARY_LAYOUT))
    f.write('#define KEY_THEME_TEXT 0x%x\n#define KEY_ACCENT_TEXT 0x%x\n#define KEY_LIBRARY_TEXT 0x%x\n#define KEY_THEME_CHECK 0x%x\n#define KEY_ACCENT_CHECK 0x%x\n#define KEY_LIBRARY_CHECK 0x%x\n' % (KEY_TEXT + KEY_CHECK))
    f.write('#define N_THEME_OPTIONS %d\n#define N_ACCENT_OPTIONS %d\n#define N_LIBRARY_OPTIONS %d\n' % (len(THEME_OPTIONS), len(ACCENT_OPTIONS), len(LIBRARY_OPTIONS)))
    f.write('#define SETTINGS_NAMES_COUNT %d\n' % NAMES_NEW_COUNT)
    f.write('/* submenu rows: action -> (group, option) */\n')
    f.write('static const struct { const char *action; uint8_t group, option; } settings_actions[%d] = {\n' % sum(len(g[1]) for g in GROUPS))
    for g, (label, options, scr, lay, action) in enumerate(GROUPS):
        for k in range(len(options)): f.write('    { "%s%d", %d, %d },\n' % (action, k, g, k))
    f.write('};\n')
    for (t, pid), data in inplace.items(): f.write(carr(csym(t, pid), data))
    for (t, pid), data in sorted(new.items()): f.write(carr(csym(t, pid), data))
    for t, tab in tables.items(): f.write(carr('settings_index_%s' % t.strip().lower(), tab))
    # the name table: Apple's 3920 {char *, pid} with ours appended, padded to 3936 with copies of entry 0
    f.write('struct e_name { const char *name; uint32_t pid; };\n')
    f.write('const struct e_name __attribute__((section(".rodata.keep"), aligned(4))) settings_names[%d] = {\n' % NAMES_NEW_COUNT)
    for k in range(NAMES_COUNT):
        p, pid = struct.unpack_from('<II', D, NAMES_TABLE - BASE + 8 * k)
        f.write('    { (const char *)0x%08x, 0x%08x },\n' % (p, pid))
    for name, pid in NAMES: f.write('    { "%s", 0x%08x },\n' % (name, pid))
    p0, pid0 = struct.unpack_from('<II', D, NAMES_TABLE - BASE)
    for k in range(NAMES_NEW_COUNT - NAMES_COUNT - len(NAMES)): f.write('    { (const char *)0x%08x, 0x%08x },\n' % (p0, pid0))
    f.write('};\n')
json.dump(plan, open(sys.argv[3], 'w'), indent=1)
print('settings: %d new resources in %d types, %d replaced; index copies %d bytes; names %d -> %d' % (
    len(new), len(tables), len(inplace), sum(len(t) for t in tables.values()), NAMES_COUNT, NAMES_NEW_COUNT))
for t, entries in types.items(): print('  %-5s +%d: %s' % (t, len(entries), ' '.join('%x' % p for p, d in entries)))
