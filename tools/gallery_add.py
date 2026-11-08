#!/usr/bin/env python3
"""Add one capture to docs/media/laptop-progress.

The PNG is the PPM's pixels unchanged (checked after writing); the entry goes
into gallery.json with its hashes and source, and a captioned section into the
folder's README after the section whose image is `after_file`. The README's
opening count and feature list are the caller's to update.

usage: gallery_add.py spec.json
spec: ppm, id, title, caption, alt, environment, source_commit, milestone,
      after_file
"""
import datetime
import hashlib
import json
import sys
from pathlib import Path

from PIL import Image

GALLERY = Path(__file__).resolve().parent.parent / 'docs/media/laptop-progress'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    spec = json.load(open(sys.argv[1]))
    ppm = Path(spec['ppm'])
    png = GALLERY / f"{spec['id']}.png"
    im = Image.open(ppm)
    im.save(png, optimize=True)
    assert Image.open(png).convert('RGB').tobytes() == im.convert('RGB').tobytes(), 'pixels changed'
    g = json.load(open(GALLERY / 'gallery.json'))
    assert not any(c['id'] == spec['id'] for c in g['captures']), f"{spec['id']} is already there"
    g['captures'].append({
        'id': spec['id'], 'phase': spec.get('phase', 'Fluid desktop'), 'title': spec['title'],
        'caption': spec['caption'], 'alt': spec['alt'], 'file': png.name,
        'capture_date': spec.get('capture_date', datetime.date.today().isoformat()),
        'environment': spec['environment'], 'synthetic_acpi_fixture': False,
        'width': im.width, 'height': im.height, 'sha256': sha(png),
        'source_capture': f'{ppm.parent.name}/{ppm.name}', 'source_sha256': sha(ppm),
        'source_commit': spec['source_commit'], 'featured': False,
        'milestone': spec['milestone'], 'historical': False})
    with open(GALLERY / 'gallery.json', 'w') as f:
        json.dump(g, f, indent=2, ensure_ascii=False)
        f.write('\n')
    readme = (GALLERY / 'README.md').read_text()
    anchor = f"]({spec['after_file']})\n"
    assert readme.count(anchor) == 1, f'no single section shows {spec["after_file"]}'
    section = f"\n### {spec['title']}\n\n{spec['caption']}\n\n![{spec['alt']}]({png.name})\n"
    (GALLERY / 'README.md').write_text(readme.replace(anchor, anchor + section))
    print(png, im.size, sha(png))


if __name__ == '__main__':
    main()
