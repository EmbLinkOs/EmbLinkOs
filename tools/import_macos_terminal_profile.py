#!/usr/bin/env python3
"""Convert a macOS Terminal.app profile into an EmbLinkOS Terminal profile.

    import_macos_terminal_profile.py --list
    import_macos_terminal_profile.py "Clear Dark" > clear-dark.conf
    import_macos_terminal_profile.py --file Exported.terminal > mine.conf

Reads ~/Library/Preferences/com.apple.Terminal.plist (or --plist PATH), or an
exported .terminal file. The output is the `key = value` format of
user/terminal/profile.h; copy it to ~/.config/terminal/profiles/ on EmbLinkOS.

Colours are archived NSColors in calibrated RGB (Apple's Generic RGB: gamma
1.8, its own primaries), device RGB, or grey. Terminal paints them through
colour management, so the values are converted to sRGB -- by macOS itself
when this runs on a Mac (JavaScript for Automation), otherwise by the same
transform done here. Calibrated (0.098, 0.114, 0.153) is sRGB (33, 39, 52),
not the (25, 29, 39) a straight x255 gives.

What has no counterpart is left out with a comment: the font, when it is not
one the image has; a macOS shell command.
"""
import argparse, json, os, plistlib, shutil, subprocess, sys

ANSI = ['Black', 'Red', 'Green', 'Yellow', 'Blue', 'Magenta', 'Cyan', 'White']
FONTS = {'meslo': 'MesloLGS Nerd Font Mono', 'dejavusansmono': 'DejaVu Sans Mono'}


# --- colour ---------------------------------------------------------------------

def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def _inv(m):
    a, b, c = m[0]; d, e, f = m[1]; g, h, i = m[2]
    A, B, C = e*i - f*h, -(d*i - f*g), d*h - e*g
    det = a*A + b*B + c*C
    return [[A/det, -(b*i - c*h)/det, (b*f - c*e)/det],
            [B/det, (a*i - c*g)/det, -(a*f - c*d)/det],
            [C/det, -(a*h - b*g)/det, (a*e - b*d)/det]]

def _to_xyz(primaries, white):
    P = [[x/y for x, y in primaries], [1.0]*3, [(1 - x - y)/y for x, y in primaries]]
    W = [white[0]/white[1], 1.0, (1 - white[0] - white[1])/white[1]]
    Pi = _inv(P)
    S = [sum(Pi[i][k]*W[k] for k in range(3)) for i in range(3)]
    return [[P[i][j]*S[j] for j in range(3)] for i in range(3)]

D65 = (0.3127, 0.3290)
GENERIC_TO_SRGB = _mul(_inv(_to_xyz([(.64, .33), (.30, .60), (.15, .06)], D65)),
                       _to_xyz([(.630, .340), (.295, .605), (.155, .077)], D65))

def _encode(v):
    v = max(0.0, min(1.0, v))
    return 12.92*v if v <= 0.0031308 else 1.055*v**(1/2.4) - 0.055

def srgb_fallback(space, comps):
    """(r, g, b) 0-255 for one colour, without macOS."""
    if space == 3:                                   # calibrated grey: Generic Gray, gamma 1.8 too
        return tuple([round(_encode(max(0.0, comps[0])**1.8)*255)]*3)
    if space == 4:                                   # device grey
        return tuple([round(max(0.0, min(1.0, comps[0]))*255)]*3)
    r, g, b = comps[:3]
    if space == 1:                                   # calibrated: Generic RGB to sRGB
        lin = [max(0.0, c)**1.8 for c in (r, g, b)]
        return tuple(round(_encode(sum(GENERIC_TO_SRGB[i][k]*lin[k] for k in range(3)))*255) for i in range(3))
    return tuple(round(max(0.0, min(1.0, c))*255) for c in (r, g, b))   # device RGB: sRGB already

def srgb_by_macos(colours):
    """Every colour converted by AppKit in one call, or None off a Mac."""
    if sys.platform != 'darwin' or not shutil.which('osascript'):
        return None
    script = '''
ObjC.import("AppKit");
function run(argv) {
  return JSON.stringify(JSON.parse(argv[0]).map(function (c) {
    var s = c[0], v = c[1], k;
    if (s == 1) k = $.NSColor.colorWithCalibratedRedGreenBlueAlpha(v[0], v[1], v[2], 1);
    else if (s == 2) k = $.NSColor.colorWithDeviceRedGreenBlueAlpha(v[0], v[1], v[2], 1);
    else if (s == 3) k = $.NSColor.colorWithCalibratedWhiteAlpha(v[0], 1);
    else k = $.NSColor.colorWithDeviceWhiteAlpha(v[0], 1);
    k = k.colorUsingColorSpace($.NSColorSpace.sRGBColorSpace);
    return [k.redComponent, k.greenComponent, k.blueComponent].map(function (x) { return Math.round(x * 255); });
  }));
}'''
    try:
        out = subprocess.run(['osascript', '-l', 'JavaScript', '-e', script, json.dumps(colours)],
                             capture_output=True, text=True, timeout=30, check=True).stdout
        return [tuple(c) for c in json.loads(out)]
    except (subprocess.SubprocessError, ValueError, OSError):
        return None

def archived_colour(data):
    """(colour space, components, alpha) from an NSKeyedArchiver'd NSColor."""
    objs = plistlib.loads(data)['$objects']
    c = objs[1]
    space = c.get('NSColorSpace', 2)
    if 'NSRGB' in c:
        vals = [float(x) for x in c['NSRGB'].rstrip(b'\x00').split()]
    elif 'NSWhite' in c:
        vals = [float(x) for x in c['NSWhite'].rstrip(b'\x00').split()]
    elif 'NSComponents' in c:                        # a custom space: take the components as sRGB
        vals = [float(x) for x in c['NSComponents'].rstrip(b'\x00').split()]
        space = 2 if len(vals) >= 3 else 4
    else:
        raise ValueError('an NSColor this does not read')
    grey = space in (3, 4)
    comps = vals[:1] if grey else vals[:3]
    alpha = vals[1] if grey and len(vals) > 1 else vals[3] if not grey and len(vals) > 3 else 1.0
    return space, comps, alpha

def archived_font(data):
    objs = plistlib.loads(data)['$objects']
    f = objs[1]
    name = f['NSName']
    if not isinstance(name, str):
        name = objs[name.data]
    return name, float(f.get('NSSize', 14.0))


# --- the profile ----------------------------------------------------------------

def convert(settings, name, fallback_only=False):
    keys = {'foreground': 'TextColor', 'background': 'BackgroundColor', 'bold': 'TextBoldColor',
            'selection': 'SelectionColor', 'cursor': 'CursorColor'}
    for i, c in enumerate(ANSI):
        keys[f'color{i}'] = f'ANSI{c}Color'
        keys[f'color{i + 8}'] = f'ANSIBright{c}Color'
    found = {k: archived_colour(settings[v]) for k, v in keys.items() if isinstance(settings.get(v), bytes)}
    order = list(found)
    rgb = None if fallback_only else srgb_by_macos([[found[k][0], found[k][1]] for k in order])
    if rgb is None:
        rgb = [srgb_fallback(found[k][0], found[k][1]) for k in order]
    hexes = {k: '#%02x%02x%02x' % c for k, c in zip(order, rgb)}

    lines = [f'# Imported from Terminal.app\'s "{name}" profile by tools/import_macos_terminal_profile.py.',
             f'name = {name}']
    note = []
    if isinstance(settings.get('Font'), bytes):
        font, size = archived_font(settings['Font'])
        squashed = font.lower().replace('-', '').replace(' ', '')
        family = next((v for k, v in FONTS.items() if squashed.startswith(k)), None)
        if family:
            lines.append(f'font = {family}')
        else:
            note.append(f'# The font was {font}, which the image does not have.')
        lines.append(f'font_size = {size:g}')
    for key, mac in (('line_spacing', 'FontHeightSpacing'), ('letter_spacing', 'FontWidthSpacing')):
        if mac in settings:
            lines.append(f'{key} = {float(settings[mac]):g}')
    # Terminal's own default for both is on
    lines.append('bold_is_bright = ' + ('yes' if settings.get('UseBrightBold', True) else 'no'))
    for k in ('foreground', 'background', 'bold', 'selection', 'cursor'):
        if k in hexes:
            lines.append(f'{k} = {hexes[k]}')
    if 'cursor' not in hexes and 'foreground' in hexes:
        lines.append(f'cursor = {hexes["foreground"]}')
    for i in range(16):
        if f'color{i}' in hexes:
            lines.append(f'color{i} = {hexes[f"color{i}"]}')
    if 'background' in found:
        lines.append(f'opacity = {found["background"][2]:.2f}')
    if 'BackgroundBlur' in settings:
        lines.append(f'blur = {float(settings["BackgroundBlur"]):.2f}')
    shape = {0: 'block', 1: 'underline', 2: 'bar'}.get(settings.get('CursorType', 0), 'block')
    lines.append(f'cursor_shape = {shape}')
    lines.append('cursor_blink = ' + ('yes' if settings.get('CursorBlink', False) else 'no'))
    if 'columnCount' in settings:
        lines.append(f'columns = {int(settings["columnCount"])}')
    if 'rowCount' in settings:
        lines.append(f'rows = {int(settings["rowCount"])}')
    # no sound: an audible bell is shown instead
    silent = not settings.get('Bell', True) and not settings.get('VisualBell', False)
    lines.append('bell = ' + ('none' if silent else 'visual'))
    if settings.get('TerminalType'):
        lines.append(f'term = {settings["TerminalType"]}')
    if settings.get('CommandString'):
        note.append(f'# The profile ran {settings["CommandString"]}, a macOS command.')
    return '\n'.join(lines + note) + '\n'


def self_test():
    """The conversion against values macOS gave for the Clear Dark profile."""
    def archive(space, text):
        return plistlib.dumps({'$archiver': 'NSKeyedArchiver', '$version': 100000, '$top': {'root': plistlib.UID(1)},
                               '$objects': ['$null', {'NSRGB': text.encode() + b'\x00', 'NSColorSpace': space,
                                                      '$class': plistlib.UID(2)},
                                            {'$classname': 'NSColor', '$classes': ['NSColor', 'NSObject']}]},
                              fmt=plistlib.FMT_BINARY)
    fake = {'BackgroundColor': archive(1, '0.09838771145 0.1138777476 0.152602838 0.95'),
            'ANSIBlackColor': archive(2, '0.208221729 0.2588729473 0.2961375701'),
            'BackgroundBlur': 0.5, 'columnCount': 120, 'rowCount': 30}
    out = convert(fake, 'Test', fallback_only=True)
    want = ['background = #212734', 'color0 = #35424c', 'opacity = 0.95', 'blur = 0.50',
            'columns = 120', 'rows = 30', 'name = Test']
    missing = [w for w in want if w not in out.splitlines()]
    if missing:
        print('FAIL: missing', missing, '\n' + out); return 1
    print('PASS: Generic RGB and device RGB to sRGB, opacity, blur, size'); return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('name', nargs='?', help='the profile to convert (default: the default profile)')
    ap.add_argument('--plist', default=os.path.expanduser('~/Library/Preferences/com.apple.Terminal.plist'))
    ap.add_argument('--file', help='an exported .terminal file instead')
    ap.add_argument('--list', action='store_true', help='list the profiles')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.file:
        with open(args.file, 'rb') as fh:
            settings = plistlib.load(fh)
        sys.stdout.write(convert(settings, args.name or settings.get('name', 'Imported')))
        return 0
    with open(args.plist, 'rb') as fh:
        prefs = plistlib.load(fh)
    profiles = prefs.get('Window Settings', {})
    if args.list:
        for n in sorted(profiles):
            print(n + ('  (default)' if n == prefs.get('Default Window Settings') else ''))
        return 0
    name = args.name or prefs.get('Default Window Settings')
    if name not in profiles:
        print(f'no profile named {name!r}; --list shows them', file=sys.stderr)
        return 1
    sys.stdout.write(convert(profiles[name], name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
