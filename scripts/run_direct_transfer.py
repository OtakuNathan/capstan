#!/usr/bin/env python3
"""Direct exclusive transfer over independent TCP business/control connections."""
import argparse
import json
import signal
from pathlib import Path
from run_demo import DemoCluster, values


def reference(result):
    return ' '.join(str(result[f'v{i}']) for i in range(7))


def run(build, output):
    checks, events = [], []
    with DemoCluster(build, output, work_delay_ms=150) as lab:
        a,b=lab.a,lab.b
        def call(client, command, expect='ok'):
            result=client.command_result(command,expect)
            events.append(dict(client=client.name,input=command,**result));return result
        call(a,'exclusive 1');first=call(a,'fetch');call(a,'keep')
        before=call(a,'summary');counters=call(a,'table_stats')
        # Stop only Table after bootstrap; both Host connections stay reusable.
        lab.table.process.send_signal(signal.SIGSTOP)
        try:
            mark=lab.host.mark();business=a.send('summary')
            lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
            transfer=a.send('transfer 2 1')
            pending,_=lab.host.wait_line(lambda s:s.startswith('TRANSFER ') and 'status=pending' in s and 'active=1' in s,mark)
            ref=' '.join(map(str,lab.identities[:3]+[1,2,first['v0'],1]))
            call(b,'claim '+ref,'pending')
            call(b,'query_transfer '+ref,'pending')
            result=a.wait_result('transfer',transfer);events.append(dict(client=a.name,input='transfer 2 1 (in-flight)',**result))
            old_result=a.wait_result('summary',business)
            assert result['status']=='ok' and old_result['status']=='ok' and result['v7']==first['v0']+1
            complete,_=lab.host.wait_line(lambda s:s.startswith('COMPLETE ') and 'active=0' in s,pending)
            assert complete>pending
            call(a,'version','invalid');call(a,'stale','stale')
            call(a,'claim '+ref,'forbidden')
            call(b,'claim '+ref);call(b,'claim '+ref,'duplicate')
            after=call(b,'summary');assert values(before)==values(after)
            retry=call(a,'retry_transfer');assert retry['v7']==result['v7']
            checks.append('real admitted business pin drains before Installed; early claim denied; old wrapper sealed; duplicate claim denied')
            call(b,'keep');back=call(b,'transfer 1 1');backref=reference(back)
            call(a,'claim '+backref)
            for _ in range(5):call(a,'version')
            superseded=call(b,'query_transfer '+ref);assert superseded['v8']==2
            call(b,'claim '+ref,'superseded');call(b,'stale','stale')
            checks.append('Table stopped during A-to-B-to-A transfers, claims and business calls; identity and resource summary preserved')
        finally:
            lab.table.process.send_signal(signal.SIGCONT)
        resumed=call(a,'table_stats');assert values(resumed)==values(counters)
        call(b,'fetch','bad_grant')
        call(a,'release_old','bad_grant')
        still=call(a,'summary');assert values(still)==values(before)
        # Table fetch must not return A's old cached grant; B is now legitimate
        # only after a new explicit administration request based on Host state.
        call(a,'exclusive 2');fetched=call(b,'fetch');assert fetched['v0']==back['v7']+1
        call(a,'claim '+backref,'superseded')
        call(b,'version');stats=call(a,'stats')
        assert stats['v0']==stats['v1'] and stats['v4']==0
        checks.append('resumed Table fetch filters current Host state; stale release cannot revoke successor; explicit admin uses current epoch')
    # Disconnect the source after actual Host acceptance, before drain/ACK.
    with DemoCluster(build, output/'disconnect',work_delay_ms=300) as lab:
        a,b=lab.a,lab.b
        a.command_result('exclusive 1');initial=a.command_result('fetch')
        mark=lab.host.mark();a.send('summary')
        lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
        a.send('transfer 2 99')
        lab.host.wait_line(lambda s:s.startswith('TRANSFER ') and 'status=pending' in s,mark)
        a.process.kill();a.process.wait(timeout=3);a.expected_exit='intentional source disconnect before transfer ACK'
        b.process.kill();b.process.wait(timeout=3);b.expected_exit='recipient offline before claim'
        lab.host.wait_line(lambda s:s.startswith('COMPLETE ') and 'active=0' in s,mark)
        replacement=lab.spawn('client_B_reconnect','capstan_client','--credential',lab.role_paths[1],
                              '--host-port',lab.host_port,'--table-port',lab.table_port)
        replacement.wait_line(lambda s:s.startswith('READY client '))
        ref=' '.join(map(str,lab.identities[:3]+[1,2,initial['v0'],99]))
        observed=replacement.command_result('query_transfer '+ref)
        assert observed['v7']==initial['v0']+1 and observed['v9']==2
        replacement.command_result('claim '+ref);replacement.command_result('version')
        stats=replacement.command_result('stats');assert stats['v0']==stats['v1'] and stats['v4']==0
        source=lab.spawn('client_A_reconnect','capstan_client','--credential',lab.role_paths[0],
                         '--host-port',lab.host_port,'--table-port',lab.table_port)
        source.wait_line(lambda s:s.startswith('READY client '))
        recovered=source.command_result('query_transfer '+ref)
        assert recovered['v7']==observed['v7'];source.command_result('claim '+ref,'forbidden')
        checks.append('source disconnected after Pending and recipient offline; pin drained; authenticated reconnect queries history and only recipient claims')
    return dict(passed=True,checks=checks,events=events,table_direct_path_delta=[0,0,0,0],work_delay_ms=150,
                evidence='real TCP; four processes; injected delay only for race orchestration')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    try:report=run(args.build_dir,args.output);code=0
    except Exception as e:report=dict(passed=False,error=repr(e));code=1
    report['command_exit_code']=code;(args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='events'},indent=2));return code
if __name__=='__main__':raise SystemExit(main())
