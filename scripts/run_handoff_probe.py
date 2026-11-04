#!/usr/bin/env python3
"""Measure handoff while a real Host-local summary is still executing.

Observed drain uses supervisor receipt times for Host event lines, not an
instrumented Host clock. Keep this measurement distinct from exact client RTT.
"""
import argparse
import csv
import json
from pathlib import Path
import time
from run_demo import DemoCluster

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,default=Path('build-release'))
p.add_argument('--output',type=Path)
p.add_argument('--samples',type=int,default=10)
a=p.parse_args()
if not 1<=a.samples<=100:p.error('samples must be 1..100')
output=a.output or a.build_dir/'handoff-drain';output.mkdir(parents=True,exist_ok=True)
rows=[]
with DemoCluster(a.build_dir,output/'processes',64*1024*1024,0) as lab:
    lab.a.command_result('exclusive 2');lab.b.command_result('fetch')
    for index in range(a.samples):
        mark=lab.host.mark();read=lab.b.send('summary')
        admit,_=lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
        transfer=lab.a.send('exclusive 1')
        pending,line=lab.host.wait_line(lambda s:s.startswith('TRANSITION '),admit)
        close_received=time.perf_counter_ns()
        if 'status=pending' in line:
            complete,_=lab.host.wait_line(lambda s:s.startswith('COMPLETE ') and 'active=0' in s,pending)
            complete_received=time.perf_counter_ns()
            observed=complete_received-close_received
        else:
            complete=-1;observed=0
        handoff=lab.a.wait_result('exclusive',transfer);summary=lab.b.wait_result('summary',read)
        assert handoff['status']=='ok' and summary['status']=='ok'
        rows.append(dict(sample=index,pending='status=pending' in line,summary_client_ns=summary['ns'],
                         handoff_client_ns=handoff['ns'],observed_drain_ns=observed,
                         close_log_index=pending,complete_log_index=complete))
        lab.a.command_result('exclusive 2');lab.b.command_result('fetch')
with (output/'raw.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
report=dict(passed=any(r['pending'] for r in rows),command_exit_code=0 if any(r['pending'] for r in rows) else 1,
            resource_bytes=64*1024*1024,artificial_delay_ms=0,samples=rows,
            measurement='Client RTT is measured in C++; observed drain is supervisor line-receipt interval from Close pending to actual Complete.',
            limitations=['Log delivery and Python scheduling affect observed drain; it is not exact Host-internal duration.',
                         'A sample without pending means summary finished before Close; observed0 is no wait, not an exact transition latency.',
                         'Small sample count, local machine only.'])
(output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(report['command_exit_code'])
