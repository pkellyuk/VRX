"""Two-model depth fusion (xmmodel), step 3: combine xm_fuse.py results.

usage (from bench/): .venv-dml/Scripts/python.exe xmmodel/xm_report.py <clip_dir> [<clip_dir> ...]
Prints markdown tables (one per DA-V2 latency): each score averaged over the clips,
with the change against ZipDepth alone. Also writes xmmodel/out/summary.md.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCORES = [('struct', 'Structure error', False), ('struct_nr', 'Near-object error', False),
          ('flicker', 'Flicker', False), ('edge', 'Edge alignment', True)]
ORDER = ['zip', 'dav2_live', 'avg', 'anchor_s24', 'curve', 'dav2_mc', 'mc_s16', 'mc_s24', 'mc_s48',
         'mc_s24_conf', 'dav2_mc_hw', 'mc_s16_hw', 'mc_s24_hw', 'mc_s48_hw',
         'mc_s16_hwv', 'mc_s24_hwv', 'mc_s48_hwv', 'zip_stab', 'mc_s16_hwv_stab', 'dav2_ideal']
LABELS = {'zip': 'ZipDepth alone (today)', 'dav2_live': 'DA-V2 alone, as live (lagged)',
          'avg': 'Naive average', 'anchor_s24': 'Anchored field (24 px), no motion comp.',
          'curve': 'Global tone curve', 'dav2_mc': 'DA-V2 alone, motion-compensated',
          'mc_s16': 'Fused, motion-comp. (16 px)', 'mc_s24': 'Fused, motion-comp. (24 px)',
          'mc_s48': 'Fused, motion-comp. (48 px)', 'mc_s24_conf': 'Fused (24 px) + flatten unsure',
          'dav2_mc_hw': 'DA-V2 alone, moved by HARDWARE vectors',
          'mc_s16_hw': 'Fused, HARDWARE vectors (16 px)', 'mc_s24_hw': 'Fused, HARDWARE vectors (24 px)',
          'mc_s48_hw': 'Fused, HARDWARE vectors (48 px)',
          'mc_s16_hwv': 'Fused, hardware + VERIFIED (16 px)', 'mc_s24_hwv': 'Fused, hardware + VERIFIED (24 px)',
          'mc_s48_hwv': 'Fused, hardware + VERIFIED (48 px)',
          'zip_stab': 'ZipDepth steadied by motion vectors (no DA-V2)',
          'mc_s16_hwv_stab': 'Fused (16 px, verified) + steadied',
          'dav2_ideal': 'DA-V2 every frame, no lag (target)'}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    clips = []
    for d in argv[1:]:
        path = os.path.join(d, 'results.json')
        if not os.path.isfile(path):
            print(f'skipping {d}: no results.json')
            continue
        with open(path) as f:
            clips.append((os.path.basename(os.path.normpath(d)), json.load(f)))
    if not clips:
        return 1

    lines = [f'Clips: {", ".join(name for name, _ in clips)}. Scores are means over the clips; '
             '(%) is the change against ZipDepth alone, where negative is better for errors and flicker '
             'and positive is better for edge alignment.', '']
    lats = sorted({lat for _, c in clips for lat in c['runs']}, key=float)
    for lat in lats:
        runs = [c['runs'][lat] for _, c in clips if lat in c['runs']]
        age = sum(r['_timing']['anchor_age_ms_mean'] for r in runs) / len(runs)
        lines += [f'### DA-V2 result ready {lat} ms after its frame (mean age when shown: {age:.0f} ms)', '',
                  '| Method | ' + ' | '.join(s[1] for s in SCORES) + ' |',
                  '|---|' + '---:|' * len(SCORES)]
        base = {k: sum(r['zip'][k] for r in runs) / len(runs) for k, _, _ in SCORES}
        for m in ORDER:
            if not all(m in r for r in runs):
                continue
            cells = []
            for k, _, _ in SCORES:
                v = sum(r[m][k] for r in runs) / len(runs)
                if m == 'zip' or base[k] == 0:
                    cells.append(f'{v:.4f}')
                else:
                    cells.append(f'{v:.4f} ({(v / base[k] - 1) * 100:+.0f}%)')
            lines.append(f'| {LABELS[m]} | ' + ' | '.join(cells) + ' |')
        lines.append('')
    text = '\n'.join(lines)
    print(text)
    os.makedirs(os.path.join(HERE, 'out'), exist_ok=True)
    with open(os.path.join(HERE, 'out', 'summary.md'), 'w', encoding='utf-8') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
