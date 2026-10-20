#!/usr/bin/env python3
"""Import The Legend of Zelda: Breath of the Wild from one XCI that holds the base game and its cumulative update.

The cartridge's secure partition carries the base Program NCA (no rights id) and the update Program NCA (BKTR RomFS,
common ticket). This reads both in place, extracts the ExeFS of the update, merges the update's RomFS over the base with
the pinned hactool, and writes a report of the SHA-256 of every output. Keys stay in memory (the title-key file for
hactool is a private temporary file). The container and NCA readers (extract_exefs.py, extract_romfs.py,
extract_update_romfs.py in this folder) are not specific to a game; only the selection of the two Program NCAs and the
title ID are BOTW's.

    python3 import_xci.py <game.xci> --keys prod.keys --hactool <hactool> --output <folder>
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cryptography.hazmat.primitives.ciphers import modes
from extract_exefs import (decrypt, read_at, require, load_keys, container_entries, common_tickets,
                           nca_inventory, nca_ctr_key, record_offset, partition_entries)
from extract_romfs import index_romfs
from extract_update_romfs import copy_range, hactool_prod_keys

TITLE_ID = '01007EF00011E000'
UPDATE_TITLE_ID = '01007EF00011E800'
UNSIGNED = re.compile(r'\s*(?:Fixed-Key Signature|NPDM Signature) \(FAIL\)')


def select_programs(records):
    """(base, update): the Program NCAs of the title, told apart by the rights id and the BKTR section."""
    programs = [(r, h) for r, h in records if r['content_type'] == 0 and r['title_id'] == TITLE_ID]
    base = [p for p in programs if p[1][0x230:0x240] == bytes(16)]
    update = [p for p in programs if p[1][0x230:0x240] != bytes(16)
              and any(p[1][0x400 + i * 512 + 4] == 4 for i in range(4))]
    require(len(base) == 1, 'Expected one base Program NCA of Breath of the Wild')
    require(len(update) <= 1, 'Expected at most one update Program NCA')
    return base[0], update[0] if update else None


def extract_exefs(f, record, hdr, ctr_key, output):
    """The ExeFS (PFS0) section of a Program NCA, every hash block checked; returns [{name, size, sha256}]."""
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
        if not start:
            continue
        fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
        require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2A0 + 32 * i], 'FS header hash mismatch')
        if fs[2:4] != b'\x01\x02':
            continue
        require(fs[4] == 3, 'Only AES-CTR ExeFS is supported')
        blocksize, levels = struct.unpack_from('<II', fs, 0x28)
        hashoff, hashsize, pfsoff, pfssize = struct.unpack_from('<QQQQ', fs, 0x30)
        secstart, secsize = start * 512, (end - start) * 512
        require(levels == 2 and blocksize > 0 and pfssize <= 512 * 1024 * 1024, 'Unsupported ExeFS hash layout')

        def section_read(relative, size):
            require(relative >= 0 and relative + size <= secsize, 'Section read out of bounds')
            absolute = secstart + relative
            aligned = absolute & ~15
            iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
            data = read_at(f, record_offset(record) + aligned, size + absolute - aligned)
            return decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:]

        hashes = section_read(hashoff, hashsize)
        require(hashlib.sha256(hashes).digest() == fs[8:40], 'ExeFS hash-table mismatch')
        pfs = section_read(pfsoff, pfssize)
        for index, pos in enumerate(range(0, pfssize, blocksize)):
            require(hashlib.sha256(pfs[pos:pos + blocksize]).digest() == hashes[index * 32:(index + 1) * 32],
                    f'ExeFS block hash mismatch: {index}')
        import io
        files = [(n, pfs[o:o + s]) for n, o, s in partition_entries(io.BytesIO(pfs), 0, len(pfs))]
        require(any(n == 'main.npdm' for n, _ in files), 'PFS0 is not ExeFS')
        output.mkdir(parents=True)
        rows = []
        for name, data in files:
            (output / name).write_bytes(data)
            rows.append(dict(name=name, size=len(data), sha256=hashlib.sha256(data).hexdigest()))
        return rows
    raise ValueError('No ExeFS section found')


def merge_romfs(game, base, update, tickets, keys_path, hactool, output):
    """Base + update RomFS through hactool's BKTR reader; all six IVFC levels must verify."""
    (base_rec, _), (update_rec, update_hdr) = base, update
    sections = [i for i in range(4) if update_hdr[0x400 + i * 512 + 4] == 4]
    require(len(sections) == 1, 'Expected one BKTR section')
    output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(output.parent).free > update_rec['size'] + 20 * (1 << 30), 'Need about 20 GiB free')
    with tempfile.TemporaryDirectory(prefix='botw-merge-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        keyfile = tmp / 'title.keys'
        fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as f:
            for rights, key in tickets.items():
                f.write(f'{rights.hex()} = {key.hex()}\n')
        copy_range(game, record_offset(update_rec), update_rec['size'], tmp / 'update.nca')
        result = tmp / 'result'
        result.mkdir()
        hactool_keys = tmp / 'hactool.keys'
        hactool_prod_keys(keys_path, hactool_keys)
        env = dict(os.environ, MNG_TITLE_KEYS=str(keyfile), MNG_BASE_RANGE_OFFSET=str(record_offset(base_rec)),
                   MNG_BASE_RANGE_SIZE=str(base_rec['size']))
        command = [str(hactool.resolve()), '-x', '-y', '--disablekeywarns', '--suppresskeys', '-k', str(hactool_keys),
                   '--basenca=' + str(game.resolve()), f'--section{sections[0]}=' + str(result / 'romfs.bin'),
                   str(tmp / 'update.nca')]
        with (result / 'merge.log').open('w') as log:
            code = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        text = (result / 'merge.log').read_text()
        checked = '\n'.join(line for line in text.splitlines() if not UNSIGNED.match(line))
        bad = code or re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', checked)
        if bad:
            (output.parent / (output.name + '-failure.log')).write_text(re.sub(r'[0-9A-Fa-f]{32,}', '<hex removed>', text))
        require(not bad, 'BKTR verification failed')
        require(all(re.search(rf'Level {i} \(GOOD\)', text) for i in range(6)), 'Missing verified IVFC levels')
        image = result / 'romfs.bin'
        with image.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        files = index_romfs(image)
        report = dict(status='pass', romfs_size=image.stat().st_size, romfs_sha256=digest, files=files,
                      integrity='BKTR merge; all six IVFC levels verified')
        (result / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        result.rename(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--hactool', type=Path, help='the pinned, patched hactool built by build_app.build_hactool')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--skip-romfs', action='store_true')
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists; choose a new directory')
    keys = load_keys(args.keys)
    summary = dict(source=str(args.game.resolve()), source_size=args.game.stat().st_size)
    with args.game.open('rb') as f:
        kind, entries = container_entries(f, args.game.stat().st_size)
        require(kind == 'xci', 'Expected an XCI')
        tickets = common_tickets(f, entries)
        base, update = select_programs(nca_inventory(f, entries, keys))
        require(update is not None, 'This XCI has no update; only a cartridge with the cumulative update is supported')
        summary['base_program'] = dict(base[0])
        summary['update_program'] = dict(update[0])
        ctr_key = nca_ctr_key(update[1], keys, tickets)
        summary['exefs_update'] = extract_exefs(f, update[0], update[1], ctr_key, args.output / 'exefs')
        summary['exefs_base'] = extract_exefs(f, base[0], base[1], nca_ctr_key(base[1], keys, {}), args.output / 'exefs-base')
    if not args.skip_romfs:
        require(args.hactool and args.hactool.is_file(), '--hactool is required for the RomFS merge')
        report = merge_romfs(args.game, base, update, tickets, args.keys, args.hactool, args.output / 'romfs')
        summary['romfs'] = dict(size=report['romfs_size'], sha256=report['romfs_sha256'], files=len(report['files']))
    (args.output / 'import-report.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: v for k, v in summary.items() if k not in ('base_program', 'update_program')}, indent=2))


if __name__ == '__main__':
    main()
