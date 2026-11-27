#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test two unprovisioned BLE companions with SDK-generated mesh fixtures."""
import asyncio,json,time,argparse
from bleak import BleakClient,BleakScanner
TX='0000fff2-0000-1000-8000-00805f9b34fb'
RX='0000fff1-0000-1000-8000-00805f9b34fb'
def varint(value):
 out=bytearray()
 while value>127: out.append((value&127)|128);value>>=7
 out.append(value);return bytes(out)
def field(tag,value):return varint(tag<<3)+varint(value)
def blob(tag,value):return varint(tag<<3|2)+varint(len(value))+value
def decode(data):
 out={};pos=0
 def number():
  nonlocal pos
  value=0;shift=0
  while True:
   b=data[pos];pos+=1;value|=(b&127)<<shift;shift+=7
   if not b&128:return value
 while pos<len(data):
  key=number();wire=key&7;tag=key>>3
  if wire==0:out[tag]=number()
  elif wire==2:size=number();out[tag]=data[pos:pos+size];pos+=size
  elif wire in (1,5):size=8 if wire==1 else 4;out[tag]=data[pos:pos+size];pos+=size
  else:raise ValueError(wire)
 return out
class Peer:
 def __init__(self,device):self.device=device;self.received=asyncio.Queue();self.buffer=bytearray();self.beacons={};self.client=BleakClient(device,timeout=20)
 def notify(self,_,chunk):
  self.buffer.extend(chunk)
  while len(self.buffer)>=4:
   if self.buffer[:2]!=b'EZ':raise AssertionError('Bad framing')
   size=int.from_bytes(self.buffer[2:4],'little')
   if len(self.buffer)<size+4:return
   packet=bytes(self.buffer[4:4+size]);del self.buffer[:4+size]
   fields=decode(packet)
   if 106 in fields:self.beacons[fields.get(1)]=decode(fields[106])
   self.received.put_nowait(packet)
 async def connect(self):await self.client.connect();await self.client.start_notify(TX,self.notify)
 async def send(self,packet):
  packet=bytes(packet);frame=b'EZ'+len(packet).to_bytes(2,'little')+packet
  for offset in range(0,len(frame),120):await self.client.write_gatt_char(RX,frame[offset:offset+120],response=True)
 async def status(self,node):
  await self.send(field(1,123)+field(2,node)+field(3,1)+blob(102,b''))
  until=time.monotonic()+10
  while time.monotonic()<until:
   packet=await asyncio.wait_for(self.received.get(),until-time.monotonic());values=decode(packet)
   if 102 in values:return decode(values[102])
  raise TimeoutError('No status')
async def main(args):
 fixtures=json.load(open(args.fixtures))
 advertisements=await BleakScanner.discover(timeout=8,return_adv=True)
 devices=[(device,advertisement.local_name or device.name) for device,advertisement in advertisements.values()]
 for device,name in devices:
  if 'EZ_' in (name or ''): print('Found',name,device.address,flush=True)
 selected=[]
 for wanted in args.name:
  device=next((d for d,name in devices if name==wanted),None)
  if not device:raise RuntimeError('Missing '+wanted)
  selected.append(Peer(device))
 try:
  for peer in selected:await peer.connect()
  for peer,fixture in zip(selected,fixtures):await peer.send(fixture['init'])
  until=time.monotonic()+60
  while time.monotonic()<until:
   states=[await p.status(int(f['node'])) for p,f in zip(selected,fixtures)]
   print('Status',states,flush=True)
   if all(s.get(4) and s.get(5) for s in states):break
   await asyncio.sleep(2)
  else:raise AssertionError('Mesh did not peer/route')
  for peer,fixture in zip(selected,fixtures):await peer.send(fixture['metadata'])
  await asyncio.sleep(.5)
  for peer,fixture in zip(selected,fixtures):
   state=await peer.status(int(fixture['node']))
   assert state.get(2) and state.get(4) and state.get(5),'Metadata update restarted the mesh'
  until=time.monotonic()+15
  while time.monotonic()<until:
   if all(p.beacons.get(int(fixtures[1-i]['node']),{}).get(3)==('Updated peer '+str(1-i)).encode() for i,p in enumerate(selected)):break
   await asyncio.sleep(.2)
  else:raise AssertionError('Updated discovery beacons not received')
  for i,peer in enumerate(selected):
   init=decode(decode(bytes(fixtures[1-i]['init']))[103])
   assert peer.beacons[int(fixtures[1-i]['node'])][4]==init[8],'Peer public key mismatch'
  print('Discovery names, public keys and metadata updates passed without losing routes',flush=True)
  async def forward(source,kind):
   recipient=selected[1-source];packet=bytes(fixtures[source][kind])
   await selected[source].send(packet)
   until=time.monotonic()+10
   while time.monotonic()<until:
    received=await asyncio.wait_for(recipient.received.get(),until-time.monotonic())
    if received==packet:break
   else:raise AssertionError('No '+kind+' delivery')
   print(kind+' forwarded byte-for-byte',source,'->',1-source,flush=True)
  for source in (0,1):
   await forward(source,'chat')
   await forward(1-source,'ack')
   await forward(source,'broadcast')
  print('PASS: secure mesh routes and bidirectional SDK-encrypted chat',flush=True)
 finally:
  for peer in selected:
   if peer.client.is_connected:await peer.client.disconnect()
if __name__=="__main__":
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--fixtures',required=True)
 parser.add_argument('--name',action='append',required=True,help='Advertised BLE names in fixture order; specify twice')
 args=parser.parse_args()
 if len(args.name)!=2:parser.error('Specify exactly two --name values')
 asyncio.run(main(args))
