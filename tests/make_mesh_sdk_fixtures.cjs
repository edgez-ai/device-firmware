// SPDX-License-Identifier: Apache-2.0
// Usage: node make_mesh_sdk_fixtures.cjs SDK_PACKAGE_DIRECTORY NODE0_HEX NODE1_HEX
const Module=require('module');const original=Module._load;
Module._load=function(name,...args){if(name==='react-native')return {};return original.call(this,name,...args);};
const path=require('path');
const [sdkRoot,node0,node1]=process.argv.slice(2);
if(!sdkRoot || !node0 || !node1) throw new Error('Provide SDK package directory and two hex mesh node IDs');
const sdkRequire=Module.createRequire(path.resolve(sdkRoot,'package.json'));
const {EdgezMeshSdk}=require(path.resolve(sdkRoot,'lib/commonjs/EdgezMeshSdk.js'));
const {x25519}=sdkRequire('@noble/curves/ed25519');
const {decodeNetworkPacket,encodeNetworkPacket}=require(path.resolve(sdkRoot,'lib/commonjs/protocol.js'));
const nodes=[node0,node1].map(node=>BigInt('0x'+node.replace(/^0x/,'')));
const configs=nodes.map((node,i)=>({identity:{userUuid:'test-'+i,userIdHigh:BigInt(i+1),userIdLow:1n,name:'Mesh test '+i,privateKey:new Uint8Array(32).fill(i+5),publicKey:x25519.getPublicKey(new Uint8Array(32).fill(i+5))},countryCode:'US',meshId:'edgez-companion-test',passphrase:'mesh-test-2026',meshBandwidthMhz:2,meshFrequencyKhz:915000}));
(async()=>{const fixtures=[];for(let i=0;i<2;i++){
 const captures=[];const sdk=new EdgezMeshSdk({transport:{invoke:async(method,args)=>{captures.push({method,...args});},subscribe:()=>()=>{}}});
 await sdk.initializeMesh(configs[i]);
 await sdk.sendTextMessage(configs[i],{nodeNum:nodes[1-i],publicKey:configs[1-i].identity.publicKey},nodes[i],'Mesh test '+i+' to '+(1-i));
 const init=captures.find(c=>c.method==='initializeMesh').packet;
 const chat=captures.find(c=>c.method==='sendNetworkPacket')?.packet ?? captures.at(-1).packet;
 const p=decodeNetworkPacket(Uint8Array.from(chat));
 const recipient=new EdgezMeshSdk({transport:{}});
 const plaintext=recipient.decryptTextMessage(configs[1-i],{nodeNum:nodes[i],publicKey:configs[i].identity.publicKey},nodes[i],nodes[1-i],p.msg.payload);
 const metadata=Array.from(encodeNetworkPacket({operation:1,interface:5,deviceSettings:{action:2,userName:'Updated peer '+i,shareLocation:true,latitude:10+i,longitude:20+i,beaconIntervalSeconds:5}}));
 const broadcast=Array.from(encodeNetworkPacket({from:nodes[i].toString(),to:'38801',operation:9,interface:5,payload:Uint8Array.from([69,90,i,0,255])}));
 fixtures.push({node:nodes[i].toString(),init,chat,plaintext,metadata,broadcast});
 }
 for(let i=0;i<2;i++) {
  const other=decodeNetworkPacket(Uint8Array.from(fixtures[1-i].chat));
  fixtures[i].ack=Array.from(encodeNetworkPacket({from:nodes[i].toString(),to:nodes[1-i].toString(),operation:3,interface:5,msg:{messageIdHigh:other.msg.messageIdHigh,messageIdLow:other.msg.messageIdLow,sequence:other.msg.sequence}}));
 }
 console.log(JSON.stringify(fixtures));})().catch(e=>{console.error(e);process.exit(1);});
