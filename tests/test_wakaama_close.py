#!/usr/bin/env python3
"""ASan regression for closing registered/pending native client contexts."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
W=ROOT/'third_party/wakaama'
class CloseContext(unittest.TestCase):
    def test_pending_transactions(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'close-client'
            sources=sorted((W/'core').glob('*.c'))+sorted((W/'data').glob('*.c'))+sorted((W/'coap').glob('*.c'))+sorted((W/'coap/er-coap-13').glob('*.c'))
            subprocess.run(['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DLWM2M_CLIENT_MODE','-DLWM2M_LITTLE_ENDIAN',
                '-DLWM2M_SUPPORT_TLV','-DLWM2M_COAP_DEFAULT_BLOCK_SIZE=256','-DLWM2M_COAP_MAX_MESSAGE_SIZE=1024','-DLWM2M_COAP_DEFAULT_MAX_RETRANSMIT=4','-DLWM2M_COAP_SEPARATE_TIMEOUT=15','-fsanitize=address,undefined','-g',
                '-I'+str(W/'include'),'-I'+str(W/'core'),'-I'+str(W/'coap'),str(ROOT/'tests/native/wakaama_close.c'),
                *map(str,sources),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main()
