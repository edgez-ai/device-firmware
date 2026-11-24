#!/usr/bin/env python3
"""Live opt-in WNM round trip. Uses real REST Write/Execute/Read; no SWD required.
Example: python3 tests/wnm_gateway.py --base http://GATEWAY:8088 --endpoint ENDPOINT
The test temporarily writes sleep duration and restores it after waking.
"""
import argparse
import json
import time
import urllib.parse
import urllib.request
import urllib.error
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True)
    parser.add_argument('--endpoint', required=True)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 15 <= args.seconds <= 120: parser.error('Use 15..120 seconds for this live test')
    root = args.base.rstrip('/')+'/api/clients/'+urllib.parse.quote(args.endpoint,safe='')+'/'
    results = {}
    def request(path, method='GET', data=None, timeout=8):
        body = None if data is None else json.dumps(data).encode()
        req = urllib.request.Request(root+path+'?timeout='+str(timeout), data=body, method=method,
                                     headers={'Content-Type':'application/json'})
        started = time.monotonic()
        try:
            with urllib.request.urlopen(req,timeout=timeout+5) as r: value=json.load(r)
        except urllib.error.HTTPError as error:
            try: value=json.load(error)
            except ValueError: value={'success':False,'http_status':error.code}
        value['_elapsed_s']=round(time.monotonic()-started,3)
        return value
    def read(rid): return request('31026/0/'+str(rid))
    def val(result):
        assert result.get('success'),result
        return int(result['content']['value'])
    results['duration_before']=read(1); original=val(results['duration_before'])
    results['cycles_before']=read(4); before=val(results['cycles_before'])
    results['write']=request('31026/0/1','PUT',{'value':args.seconds,'type':'integer'})
    assert results['write'].get('success'),results
    try:
        results['execute']=request('31026/0/0','POST')
        assert results['execute'].get('success'),results
        print('Execute acknowledged:',results['execute'],flush=True)
        start=time.monotonic(); time.sleep(4)
        results['during']=request('3/0/13',timeout=3)
        if results['during'].get('success'):
            results['entry_result']=read(3)
            results['state']=read(2)
            raise AssertionError('Device stayed reachable: WNM entry did not complete; '+str(results))
        print('Read during WNM timed out:',results['during'],flush=True)
        time.sleep(max(0,args.seconds+4-(time.monotonic()-start)))
        for attempt in range(4):
            results['after']=request('3/0/13',timeout=8)
            if results['after'].get('success'): break
            time.sleep(1)
        assert results['after'].get('success'),results
        # Immediate post-TX reads can hide a chip that never resumes DTIM RX.
        # Leave it quiet past neighbor reachability probes, then read again.
        time.sleep(12)
        results['after_idle']=request('3/0/13',timeout=8)
        assert results['after_idle'].get('success'),results
        results['sleep']={str(i):read(i) for i in range(1,8)}
        assert val(results['sleep']['2'])==0,results
        assert val(results['sleep']['3'])==0,results
        assert val(results['sleep']['4'])==before+1,results
        assert args.seconds*1000 <= val(results['sleep']['7']) < (args.seconds+3)*1000,results
        results['write_after']=request('31026/0/1','PUT',{'value':original,'type':'integer'})
        assert results['write_after'].get('success'),results
        print('Wake recovered: clock Read, duration Write, one completed cycle; diagnostics:',results['sleep'],flush=True)
    finally:
        if args.output: args.output.write_text(json.dumps(results,indent=2)+'\n')
if __name__=='__main__': main()
