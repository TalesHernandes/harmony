"""Inspect the shipped font without installing it or requiring Python packages."""
import hashlib
import json
from pathlib import Path
import struct

root = Path(__file__).parent
font = root / 'emoji' / 'TwemojiMozilla.ttf'
data = font.read_bytes()

def unpack(fmt, offset):
    return struct.unpack_from('>' + fmt, data, offset)

tables = {}
for index in range(unpack('H', 4)[0]):
    offset = 12 + index * 16
    tables[data[offset:offset + 4].decode('ascii')] = unpack('II', offset + 8)

names = {}
offset = tables['name'][0]
count, strings = unpack('HH', offset + 2)
for index in range(count):
    platform, encoding, language, name_id, length, position = unpack('HHHHHH', offset + 6 + index * 12)
    if platform == 3:
        start = offset + strings + position
        names[name_id] = data[start:start + length].decode('utf-16-be')

cmap = tables['cmap'][0]
groups = []
for index in range(unpack('H', cmap + 2)[0]):
    platform, encoding, position = unpack('HHI', cmap + 4 + index * 8)
    subtable = cmap + position
    if platform == 3 and encoding == 10 and unpack('H', subtable)[0] == 12:
        groups = [unpack('III', subtable + 16 + item * 12) for item in range(unpack('I', subtable + 12)[0])]

def glyph(codepoint):
    for first, last, first_glyph in groups:
        if first <= codepoint <= last:
            return first_glyph + codepoint - first
    return 0

colr = tables['COLR'][0]
version, base_count, base_offset, layer_offset, layer_count = unpack('HHIIH', colr)
layers = dict((glyph_id, count) for glyph_id, first, count in
              (unpack('HHH', colr + base_offset + index * 6) for index in range(base_count)))
emoji = '🔥😀😃😄😁😆😅😂🙂🙃😉😊😍😘😎🤔😭😡👍👎👏🙌🙏🎉🚀⭐✨💙💚💜'
coverage = [{'emoji': character, 'glyph': glyph(ord(character)),
             'color_layers': layers.get(glyph(ord(character)), 0)} for character in emoji]
digest = hashlib.sha256(data).hexdigest().upper()
result = {
    'passed': digest == '6D90152EE0D29E82FE2A87793AF5AA4B7AD13E6538360889E141E81ED299EE8E'
              and version == 0 and 'CPAL' in tables and all(item['color_layers'] > 0 for item in coverage),
    'family': names.get(1, 'não encontrado'), 'version': names.get(5, 'não encontrado'),
    'bytes': len(data), 'mib': len(data) / 1048576, 'sha256': digest,
    'colr_version': version, 'cpal_present': 'CPAL' in tables,
    'source': 'C:/Program Files/Mozilla Firefox/fonts/TwemojiMozilla.ttf',
    'code_license': 'Apache-2.0', 'artwork_license': 'CC-BY-4.0',
    'coverage': coverage,
}
(root / 'fonte.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
print(json.dumps({key: value for key, value in result.items() if key != 'coverage'}, ensure_ascii=True, indent=2))
if not result['passed']:
    raise SystemExit('Font integrity or color coverage failed.')
