#!/usr/bin/env python3
"""Exercise scalar dispatch modes and bounded history over real TCP."""
import argparse
import json
from pathlib import Path
from run_demo import DemoCluster, values
from run_direct_transfer import reference


def run(build, output):
    results = []
    for inline in (0, 1):
        with DemoCluster(build, output / f'inline-{inline}', 1024 * 1024, 0,
                         host_extra_args=['--inline-version', inline,
                                          '--trace-requests', 0,
                                          '--history-limit', 8]) as lab:
            a, b = lab.a, lab.b
            a.command_result('shared')
            a.command_result('fetch')
            b.command_result('fetch')
            assert values(a.command_result('version')) == [0]
            assert values(b.command_result('version')) == [0]
            before = values(a.command_result('summary'))
            a.command_result('exclusive 1')
            a.command_result('fetch')
            a.command_result('keep')
            assert values(a.command_result('add 0 4 3')) == [1]
            middle = values(a.command_result('summary'))
            assert middle[1] == (before[1] + 12) % (1 << 64)
            result = a.command_result('transfer 2 1')
            b.command_result('claim ' + reference(result))
            assert values(b.command_result('version')) == [1]
            a.command_result('stale', 'stale')
            assert values(b.command_result('add 4 8 5')) == [2]
            after = values(b.command_result('summary'))
            assert after[0] == before[0] and after[3] == 2
            assert after[1] == (before[1] + 32) % (1 << 64)
            b.command_result('version', 'exhausted')
            # Transferring authority must not forget old business requests.
            back = b.command_result('transfer 1 1')
            a.command_result('claim ' + reference(back))
            a.command_result('version', 'exhausted')
            stats = values(a.command_result('stats'))
            assert stats[0] == stats[1] == 8 and stats[2] == 3 and stats[4] == 0
            assert not any(line.startswith(('ADMIT ', 'COMPLETE ', 'REJECT '))
                           for line in lab.host.lines)
            results.append({'inline_version': inline, 'host_stats': stats,
                            'quiet_trace': True, 'history_survives_transfer': True})
    report = {'passed': True, 'modes': results}
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    print(json.dumps(run(args.build_dir, args.output), indent=2))
