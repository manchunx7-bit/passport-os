const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('main/apps/home/phone_setup.html','utf8');
const algorithm=html.slice(html.indexOf('function portraitLocalContrast('),html.indexOf('async function imageFile('));
assert.ok(!html.includes('portraitStyleClassic'),'style ids must not be hardcoded in the page');
const elements={portraitBackground:{checked:false},portraitBrightness:{value:'0'},portraitColor:{value:'50'}};
const themePalettes=[[0x254d36,0x4b8060,0x80b795,0xa8edbb],[0x484d43,0x797d69,0xb6b7a0,0xeee7d6],[0x374543,0x637771,0xadbfb1,0xf1eee6],[0x343c55,0x657593,0xb3c5d8,0xf4f4f6],[0x343740,0x646978,0xa8aeb8,0xf5f3ee]];
const portraitStyles=[{id:0,label:'原图',palette:[0,0,0,0]},...themePalettes.map((palette,i)=>({id:i+1,label:'主题'+(i+1),palette}))];
const context=vm.createContext({$:id=>elements[id],PORTRAIT:{w:96,h:138},portrait:null,portraitStyle:0,portraitStyleInputs:[],portraitPreparedCache:null,portraitPalette:[0,1,2,255],portraitStyles,fixture:null});
vm.runInContext(algorithm,context);
function rgba(values){return Uint8ClampedArray.from(values.flatMap(v=>[v,v,v,255]))}
const tones=rgba([0,80,150,255]);
const prepared=context.portraitPrepare(tones,4,1,false);
assert.equal(prepared.mask[0],1,'opaque black must remain independent of transparency');
const transparent=rgba([255,80,150,255]);transparent[3]=0;
assert.equal(context.portraitPrepare(transparent,4,1,false).mask[0],0);
const solid=rgba(new Array(64).fill(255));
for(const p of [27,28,35,36])solid.set([110,110,110,255],p*4);
const cut=context.portraitPrepare(solid,8,8,true);
assert.equal(cut.mask[0],0);assert.equal(cut.mask[27],1);
assert.equal(context.portraitPrepare(solid,8,8,false).mask[0],1);
const blank=context.portraitPrepare(rgba(new Array(64).fill(255)),8,8,true);
assert.ok(Array.from(blank.mask).every(v=>v===0));
function lowHigh(style){
 elements.portraitBrightness.value='-48';context.setPortraitStyle(style);const low=Buffer.from(context.portraitPacked());
 elements.portraitBrightness.value='48';context.setPortraitStyle(style);const high=Buffer.from(context.portraitPacked());
 elements.portraitBrightness.value='0';return [low,high];
}
context.fixture=Uint8ClampedArray.from(Array.from({length:96*138},(_,i)=>i*37).flatMap((v,i)=>[v&255,(v*3+i)&255,(v*7+2*i)&255,255]));
context.fixture[3]=0;
vm.runInContext('function portraitPixels(){return fixture}',context);
for(const style of [1,2,3,4,5]){
 const [low,high]=lowHigh(style);
 const levels=p=>Array.from({length:96*138},(_,n)=>(p[40+(n>>1)]>>(n&1?0:4))&15);
 const lowLevels=levels(low),highLevels=levels(high);
 assert.ok(highLevels.every((v,i)=>v>=lowLevels[i]),'brightness raises theme levels');
}
function paletteColors(packed){
 const out=[];
 for(let i=1;i<16;i++){const c=packed[8+(i-1)*2]|(packed[9+(i-1)*2]<<8);out.push([Math.round(((c>>11)&31)*255/31),Math.round(((c>>5)&63)*255/63),Math.round((c&31)*255/31)])}
 return out;
}
const rgb=value=>[(value>>16)&255,(value>>8)&255,value&255];
context.setPortraitStyle(0);
elements.portraitColor.value='50';
const color=context.portraitPacked();
assert.equal(color.length,6664);
assert.deepEqual(Array.from(color.slice(0,8)),[80,84,51,54,0,50,0,0]);
assert.equal(context.portraitColor(color,0),0,'transparent pixel uses the homepage background');
assert.equal((color[40]>>4)&15,0,'first transparent pixel keeps index zero');
const midPalette=paletteColors(color);
assert.ok(midPalette.some(c=>c[0]!==c[1]||c[1]!==c[2]),'default style keeps image colors');
elements.portraitColor.value='0';context.setPortraitStyle(0);
const grayPalette=paletteColors(context.portraitPacked());
assert.ok(grayPalette.every(([r,g,b])=>Math.max(r,g,b)-Math.min(r,g,b)<=8),'zero retention removes chroma');
elements.portraitColor.value='100';context.setPortraitStyle(0);
const fullPalette=paletteColors(context.portraitPacked());
assert.ok(fullPalette.some(([r,g,b])=>Math.max(r,g,b)-Math.min(r,g,b)>16),'full retention restores chroma');
const spread=p=>p.reduce((m,c)=>Math.max(m,Math.max(...c)-Math.min(...c)),0);
assert.ok(spread(fullPalette)>spread(midPalette),'higher retention widens the palette');
elements.portraitColor.value='50';
const stylePacks=[];
for(let style=1;style<=5;style++){
 context.setPortraitStyle(style);
 const packed=context.portraitPacked();stylePacks.push(Buffer.from(packed));
 assert.equal(packed[4],style);
 const palette=paletteColors(packed),theme=themePalettes[style-1];
 for(let i=0;i<4;i++)assert.ok(palette[i].every((v,k)=>Math.abs(v-rgb(theme[i])[k])<=8),'theme palette '+style+' tone '+i);
 assert.ok(Array.from({length:96*138},(_,n)=>{const x=(packed[40+(n>>1)]>>(n&1?0:4))&15;return x<=4}).every(Boolean),'theme styles use four tones plus transparent');
}
assert.equal(new Set(stylePacks.map(p=>p.subarray(8,40).toString('hex'))).size,5,'each style stores its own palette');
context.setPortraitStyle(0);
const deterministic=Buffer.from(context.portraitPacked());
assert.deepEqual(Array.from(context.portraitPacked()),Array.from(deterministic),'preview and save are deterministic');
assert.equal(context.portraitColor(deterministic,0),0);
context.setPortraitStyle(999);assert.equal(context.portraitStyle,0,'unknown style falls back to the default');
const textureOutputs={};
for(const style of [1,0]){
 context.setPortraitStyle(style);
 const variants=[];
 for(const texture of [1,2,3]){
  context.setPortraitTexture(texture);
  const packed=Buffer.from(context.portraitPacked());
  variants.push(packed);
  if(style===1&&texture===1){
   assert.ok(Array.from({length:96*138},(_,n)=>{const x=(packed[40+(n>>1)]>>(n&1?0:4))&15;return x===0||x===4}).every(Boolean),'dot-matrix uses highlight + transparent');
  }
 }
 if(style===0){
  assert.deepEqual(variants[0],variants[1],'original style ignores dot vs flat');
  assert.deepEqual(variants[1],variants[2],'original style ignores flat vs hybrid');
 }else{
  assert.notDeepEqual(variants[0],variants[1],'dot vs flat differs for style '+style);
  assert.notDeepEqual(variants[1],variants[2],'flat vs hybrid differs for style '+style);
 }
 textureOutputs[style]=variants;
}
context.setPortraitTexture(999);assert.equal(vm.runInContext('portraitTexture',context),3,'unknown texture falls back to hybrid');
console.log('Portrait colors: PT36 packing, 15-color palette, retention slider, 5 themes, 3 textures, masks, brightness and determinism: PASS');
