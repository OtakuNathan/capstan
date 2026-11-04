#!/usr/bin/env python3
"""Real TCP: disconnect a retained control reply while an old write drains."""
import argparse
import json
from pathlib import Path
from run_demo import DemoCluster

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
report={'passed':False,'checks':[]}
try:
    with DemoCluster(a.build_dir,a.output/'processes',1024*1024,1000) as lab:
        malformed=lab.spawn('wire_faults','wire_fault_probe','--config',lab.config,
            '--host-port',lab.host_port,'--table-port',lab.table_port)
        malformed.wait_line(lambda s:s=='WIRE_FAULTS completed=21 alive=1')
        assert malformed.process.wait(timeout=3)==0
        report['checked_wire_cases']=14
        lab.a.command_result('exclusive 2');lab.b.command_result('fetch')
        mark=lab.host.mark();write=lab.b.send('add 0 1 7')
        admitted,_=lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
        def probe(name,operation='set',expect='ok',destroy=0):
            return lab.spawn(name,'control_probe','--config',lab.config,
                '--host-port',lab.host_port,'--operation',operation,'--expect',expect,
                '--tx',1000,'--epoch',1,'--destroy',destroy)
        abandoned=probe('control_abandoned')
        closed,_=lab.host.wait_line(lambda s:s.startswith('TRANSITION tx=1000 ') and 'status=pending' in s,admitted)
        # Kill after Host retained its reply, while old operation is still live.
        abandoned.process.kill();abandoned.process.wait(timeout=3)
        abandoned.expected_exit='intentional SIGKILL of control caller during Drain'
        state=lab.a.command_result('stats');assert state['v4']==1 and state['v5']==1
        pending=probe('control_query_pending','query','pending')
        pending.wait_line(lambda s:s=='CONTROL status=pending');assert pending.process.wait(timeout=3)==0
        conflict=probe('control_conflict',expect='conflict',destroy=1)
        conflict.wait_line(lambda s:s=='CONTROL status=conflict');assert conflict.process.wait(timeout=3)==0
        duplicate=probe('control_duplicate')
        lab.host.wait_line(lambda s:s.startswith('TRANSITION tx=1000 ') and 'status=pending' in s,closed+1)
        duplicate.wait_line(lambda s:s=='CONTROL status=ok tx=1000 epoch=2')
        assert duplicate.process.wait(timeout=3)==0
        assert lab.b.wait_result('add',write)['status']=='ok'
        completed,_=lab.host.wait_line(lambda s:s.startswith('COMPLETE ') and 'active=0' in s,closed)
        state=lab.a.command_result('stats')
        assert state['v0']==state['v1']==1 and state['v4']==0 and state['v5']==2
        for name,operation in [('control_query_installed','query'),('control_retry','set')]:
            client=probe(name,operation)
            client.wait_line(lambda s:s=='CONTROL status=ok tx=1000 epoch=2')
            assert client.process.wait(timeout=3)==0
        lab.b.command_result('version','stale')
        report['checks']=[
            'control connection killed after Close leaves old operation active and old epoch installed',
            'query during Drain returns Pending; same transaction with changed target returns Conflict',
            'duplicate SetAuthority waits for actual completion and returns exact Installed ACK',
            'dead original connection tolerates late completion; Host remains usable',
            'query/retry after lost ACK return same epoch; old grant is stale; business write executes once']
        report['host_final']=state
        report['log_order']={'admit':admitted,'close':closed,'complete':completed}
    report['passed']=True
except Exception as error:
    report['error']=repr(error)
report['exit_code']=0 if report['passed'] else 1
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
raise SystemExit(report['exit_code'])
