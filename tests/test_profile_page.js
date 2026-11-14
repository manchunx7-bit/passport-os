// Run the actual offline phone flow, including network failures and partial saves.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('main/apps/home/phone_setup.html','utf8');
const cityCatalog=JSON.parse(fs.readFileSync('main/apps/home/cities.json','utf8'));
const script=html.match(/<script>\r?\n([\s\S]*?)<\/script>/)[1];
// The gzipped page receives its API token from the pt cookie set with it.
{const line=script.split('\n').find(l=>l.includes('pt=('));for(const [cookie,t] of [['a=1; pt=4294967295','4294967295'],['xpt=9','']]){const w={};vm.runInNewContext(line,{window:w,document:{cookie}});assert.equal(w.__T,t)}}
assert.ok(!/<(?:script|img|link)\b[^>]*(?:src|href)=["']https?:|url\(["']?https?:/i.test(html),'the captive page must not load internet resources');
assert.match(html,/id="copyVoiceTutorial"[^>]*>复制教程链接/);
assert.ok(!html.includes('AI-Passport-Hotkey-Windows.exe'));
assert.match(html,/href="https:\/\/github.com\/manchunx7-bit\/ai-passport-muse"/,'Muse project is discoverable from phone setup');
const elements=new Map(),requests=[],timers=new Map();let nextTimer=1;
class Element {
 constructor(){this.value='';this.textContent='';this.style={};this.dataset={};this.options=[];this.classList={toggle(){}};this.type='password';this.files=[];}
 add(o){this.options.push(o)} replaceChildren(...o){this.options=o;this.value=''}
 appendChild(o){this.options.push(o);return o} click(){this.onclick?.()} setAttribute(k,v){this[k]=v} removeAttribute(k){delete this[k]} focus(){} select(){} setSelectionRange(){} setPointerCapture(){}
 get selectedOptions(){return this.options.filter(o=>o.value===this.value)}
 getContext(){return {drawImage(){},fillRect(){},putImageData(){},getImageData:(x,y,w,h)=>({data:new Uint8ClampedArray(w*h*4).fill(255)}),createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)})}}
 toBlob(cb,type,quality){assert.equal(type,'image/jpeg');qualityAttempts.push(quality);cb(new Blob([new Uint8Array(typeof blobBytes==='function'?blobBytes(quality):blobBytes)],{type}))}
}
const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id)};
let imageMode='ok';
class Image {constructor(){this.naturalWidth=240;this.naturalHeight=320}set src(v){this._src=v;const mode=imageMode;if(mode!=='stall')setImmediate(()=>mode==='error'?this.onerror?.():this.onload?.())}get src(){return this._src}}
let state='idle',saved=false,failShare=false,denyFinish=false,emptyScan=true,failScan=false;
let connectError='';
let avatarError='',avatarDeviceCode='ESP_FAIL';
let museTokenSet=false,musePaired=false,museProxyHost='',museProxyPort=0,failMuseSave=false,failMuseLoad=false,museGate=null;
let failCities=false,cityGate=null;let savedProfile={name:'测试',signature:'已有签名',address:'原地址',city:'自定义城市',lat:'22.3',lon:'113.6',avatar:true,share_image:true};let gallery=[{slot:0,bytes:12000}],blobBytes=20000,qualityAttempts=[];
const phoneWindow={__T:314159,scrollTo(){},addEventListener(){},isSecureContext:false,crypto:require('node:crypto').webcrypto,location:{origin:'http://192.168.4.1'}};
const phoneNavigator={};
class TestURL extends URL{static createObjectURL(){return 'blob:fixture'}static revokeObjectURL(){}}
const context=vm.createContext({document:{getElementById:get,createElement:()=>new Element()},window:phoneWindow,navigator:phoneNavigator,Option:class{constructor(text,value){this.text=text;this.value=value;this.dataset={}}},Image,TextEncoder,Uint8Array,Uint8ClampedArray,Blob,AbortController,URL:TestURL,URLSearchParams,setTimeout(fn,ms){const id=nextTimer++;timers.set(id,{fn,ms});return id},clearTimeout:id=>timers.delete(id),fetch:async(url,options)=>{
 requests.push({url,options});let result={},ok=true;
 if(url.startsWith('/api/cities')){if(cityGate)await cityGate;if(failCities)throw new Error('offline');result=cityCatalog;}
 if(url.startsWith('/api/muse')){if(options.method==='POST'){if(failMuseSave){ok=false;result={error:'muse_config'}}else{const data=JSON.parse(options.body);if(data.sdk_token){museTokenSet=true;musePaired=false}if('proxy_host' in data){museProxyHost=data.proxy_host;museProxyPort=data.proxy_port}result={ok:true,token_set:museTokenSet,paired:musePaired,proxy_host:museProxyHost,proxy_port:museProxyPort}}}else{if(museGate)await museGate;if(failMuseLoad)throw new Error('offline');result={token_set:museTokenSet,paired:musePaired,proxy_host:museProxyHost,proxy_port:museProxyPort}}}
 if(url.startsWith('/api/profile')&&!options.method)result=savedProfile;
 if(url.startsWith('/api/avatar')&&options.method==='POST'&&avatarError){ok=false;result={error:avatarError,code:avatarDeviceCode};}
 if(url.startsWith('/api/setup'))result={state,saved_network:saved,saved_networks:saved?['Absent Wi-Fi','Test Wi-Fi']:[],ssid:'Test Wi-Fi'};
 if(url.startsWith('/api/connect')&&connectError){ok=false;result={error:connectError};}
 if(url.startsWith('/api/networks')){if(failScan)throw new Error('offline');result=emptyScan?[]:[{ssid:'Test Wi-Fi',secure:true}];}
 if(url.startsWith('/api/cards')){if(options.method==='POST'){const order=JSON.parse(options.body).order;gallery=order.map(slot=>gallery.find(c=>c.slot===slot));}else result={max:6,per_image_bytes:24576,storage_free:1900000,storage_reserve:65536,images:gallery.map(c=>({...c}))};}
 if(url.startsWith('/api/share')&&options.method==='POST'){if(failShare){ok=false;result={error:'store'}}else{const slot=Number(new URL('http://device'+url).searchParams.get('slot')||0);gallery=gallery.filter(c=>c.slot!==slot).concat({slot,bytes:options.body.size});}}
 if(url.startsWith('/api/finish')&&denyFinish){ok=false;result={error:'network_required'}}
 return {ok,json:async()=>result};
}});
vm.runInContext(script,context);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const posts=()=>requests.filter(r=>r.options.method==='POST');
(async()=>{
 await flush();await flush();
 let copiedText='';
 context.document.execCommand=cmd=>{assert.equal(cmd,'copy');copiedText=get('voiceTutorialLink').value;return true};
 await get('copyVoiceTutorial').onclick();assert.equal(copiedText,'https://github.com/manchunx7-bit/passport-os');assert.equal(get('voiceTutorialLink').hidden,true);
 context.document.execCommand=()=>false;
 await get('copyVoiceTutorial').onclick();assert.equal(get('voiceTutorialLink').hidden,false);assert.match(get('voiceCopyStatus').textContent,/长按/);
 phoneWindow.isSecureContext=true;phoneNavigator.clipboard={writeText:async text=>{copiedText=text}};
 await get('copyVoiceTutorial').onclick();assert.equal(get('voiceTutorialLink').hidden,true);
 phoneNavigator.clipboard.writeText=async()=>{throw Error('denied')};
 await get('copyVoiceTutorial').onclick();assert.equal(get('voiceTutorialLink').hidden,false);
 phoneWindow.isSecureContext=false;

 assert.match(get('scanStatus').textContent,/等待扫描结果/);
 const retry=[...timers.entries()].find(([,t])=>t.ms===1500);assert.ok(retry,'empty first result must schedule a retry');
 emptyScan=false;timers.delete(retry[0]);await retry[1].fn();assert.match(get('scanStatus').textContent,/已找到 1/);
 get('network').value='Test Wi-Fi';failScan=true;await context.scan();assert.equal(get('network').value,'Test Wi-Fi','failed refresh preserves selection');
 failScan=false;await context.scan();
 await get('save').onclick();assert.equal(posts().length,0,'first boot cannot complete without Wi-Fi');
 get('network').value='Test Wi-Fi';get('password').value='short';await get('connect').onclick();assert.equal(posts().length,0);
 get('password').value='  secret with spaces  ';await get('connect').onclick();
 let req=posts().at(-1);assert.equal(req.url,'/api/connect?t=314159');assert.equal(JSON.parse(req.options.body).password,'  secret with spaces  ');
 state='failed';await context.poll();assert.equal(get('connect').disabled,false);assert.match(get('networkStatus').textContent,/检查密码/);
 await get('connect').onclick();state='connected';await context.poll();assert.equal(get('step2').hidden,false);assert.equal(get('password').value,'');
 get('name').value='新名字';
 get('file').files=[{size:1000}];await get('file').onchange();
 get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 failShare=true;await get('save').onclick();assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,0,'failed image must not close hotspot');assert.equal(get('save').disabled,false);
 req=posts().find(r=>r.url.startsWith('/api/profile'));let profile=JSON.parse(req.options.body);assert.equal(profile.name,'新名字');assert.equal(profile.address,'原地址','address field removed; saved value is preserved');assert.equal(profile.city,'自定义城市');assert.equal(profile.lat,'22.3','preserve custom weather coordinates');
 req=posts().find(r=>r.url.startsWith('/api/avatar'));assert.equal(req.options.body.byteLength,4976);assert.equal(req.options.body[4],3);
 // Failed avatar uploads retain the page and pending image for a later retry.
 for(const [code,message] of [['avatar_size',/长度不匹配/],['avatar_recv',/传输中断/],['avatar_store',/原头像已保留/]]){
  avatarError=code;const finishesBefore=posts().filter(r=>r.url.startsWith('/api/finish')).length;
  await get('save').onclick();assert.match(get('saveStatus').textContent,message);
  assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,finishesBefore);
  assert.equal(get('save').disabled,false);assert.equal(get('back').disabled,false);
 }
 avatarDeviceCode='ESP_ERR_NO_MEM';await get('save').onclick();assert.match(get('saveStatus').textContent,/设备内存不足/);
 await assert.rejects(context.api('/api/avatar',new Uint8Array(4976)),error=>error.path==='/api/avatar'&&error.code==='avatar_store'&&error.deviceCode==='ESP_ERR_NO_MEM');
 avatarError='';avatarDeviceCode='ESP_FAIL';
 const writesBeforePreset=posts().length;
 for(const [id,preset] of [['portraitPresetDot',1],['portraitPresetFlat',2],['portraitPresetHybrid',3]]){get(id).checked=true;get(id).onchange();assert.equal(context.portraitPacked()[4],preset)}
 assert.equal(posts().length,writesBeforePreset,'changing presets only updates preview');
 failShare=false;denyFinish=true;await get('save').onclick();assert.match(get('saveStatus').textContent,/连接 Wi-Fi/);
 denyFinish=false;await get('save').onclick();assert.equal(get('step3').hidden,false);assert.equal(get('doneName').textContent,'新名字');
 // A reload during verification resumes polling instead of allowing a duplicate request.
 state='connecting';saved=true;await context.load();assert.equal(get('connect').disabled,true);assert.equal(get('reuse').hidden,false);
 assert.equal(get('savedField').hidden,false);assert.equal(get('savedNetwork').disabled,true);
 state='failed';await context.poll();get('savedNetwork').value='Test Wi-Fi';
 await get('reuse').onclick();const timeBody=JSON.parse(posts().at(-1).options.body);
 assert.equal(timeBody.saved,true);assert.equal(timeBody.ssid,'Test Wi-Fi');
 assert.ok(Number.isInteger(timeBody.epoch)&&Math.abs(timeBody.epoch-Date.now()/1000)<5);
 assert.equal(timeBody.tz_minutes,-new Date().getTimezoneOffset() || 0);
 state='failed';await context.poll();get('savedNetwork').value='';connectError='saved_not_found';
 const beforeRejectedTimer=nextTimer;
 await get('reuse').onclick();assert.equal(get('reuse').disabled,false);assert.match(get('networkStatus').textContent,/已保存网络/);
 assert.equal([...timers.entries()].some(([id,t])=>id>=beforeRejectedTimer&&t.fn===context.poll),false,'an explicit rejection must not start polling');
 connectError='';get('keepSaved').onclick();assert.equal(get('step2').hidden,false);
 assert.match(get('connectionSummary').textContent,/完成后自动重连/);
 const previousMusePosts=posts().filter(r=>r.url.startsWith('/api/muse')).length;
 get('museToken').value='';await get('saveMuse').onclick();
 get('museToken').value='wrong';await get('saveMuse').onclick();
 assert.equal(posts().filter(r=>r.url.startsWith('/api/muse')).length,previousMusePosts,'invalid SDK token is not posted');
 get('museToken').value='mgst_test_fixture_only_12345';await get('saveMuse').onclick();
 const museRequest=posts().at(-1);assert.equal(museRequest.url,'/api/muse?t=314159');
 assert.equal(JSON.parse(museRequest.options.body).sdk_token,'mgst_test_fixture_only_12345');
 assert.equal(get('museToken').value,'');assert.equal(get('saveMuse').disabled,false);
 assert.match(get('museSaved').textContent,/Token 已保存，无需重新填写/);
 assert.match(get('museToken').placeholder,/已保存/);assert.equal(get('useSavedMuse').hidden,false);
 musePaired=true;await context.loadMuse();
 let proxyPosts=posts().filter(r=>r.url.startsWith('/api/muse')).length;
 get('museProxyHost').value='127.0.0.1';get('museProxyPort').value='7897';await get('saveMuse').onclick();
 get('museProxyHost').value='192.168.1.999';await get('saveMuse').onclick();
 get('museProxyHost').value='192.168.1.20';get('museProxyPort').value='0';await get('saveMuse').onclick();
 assert.equal(posts().filter(r=>r.url.startsWith('/api/muse')).length,proxyPosts,'invalid proxy addresses and port are not posted');
 get('museProxyPort').value='7897';await get('saveMuse').onclick();
 let proxyBody=JSON.parse(posts().at(-1).options.body);assert.deepEqual(proxyBody,{proxy_host:'192.168.1.20',proxy_port:7897});
 assert.equal(get('museProxyHost').value,'192.168.1.20');assert.equal(get('museState').textContent,'已配对');
 assert.match(get('museStatus').textContent,/重试连接/);
 // Reopening the page recovers the stored configuration without exposing a token.
 let savedWrites=posts().length;
 vm.runInContext('museConfigured=false;museLoaded=false;museProxyEdited=false',context);
 get('museProxyHost').value='';get('museProxyPort').value='';await context.loadMuse();
 assert.equal(get('museProxyHost').value,'192.168.1.20');assert.equal(get('museProxyPort').value,7897);
 assert.equal(get('museToken').value,'');assert.match(get('museSaved').textContent,/192\.168\.1\.20:7897/);
 assert.equal(posts().length,savedWrites,'loading saved configuration does not rewrite credentials or pairing');
 get('museToken').value='mgst_unsaved_fixture_only';get('museProxyPort').value='8080';get('museProxyPort').oninput();
 await get('useSavedMuse').onclick();
 assert.equal(get('museToken').value,'');assert.equal(get('museProxyPort').value,7897);
 assert.equal(posts().length,savedWrites,'reuse only restores saved values');
 // A late response must not replace a proxy the user has already edited.
 let releaseMuse;museGate=new Promise(resolve=>{releaseMuse=resolve});
 const pendingMuse=context.loadMuse();get('museProxyPort').value='8080';get('museProxyPort').oninput();
 assert.equal(get('saveMuse').disabled,true);assert.equal(context.loadMuse(),pendingMuse,'concurrent loads share one request');
 releaseMuse();await pendingMuse;museGate=null;
 assert.equal(get('museProxyPort').value,'8080','late reads preserve unsaved edits');
 await get('useSavedMuse').onclick();
 failMuseLoad=true;await context.loadMuse();
 assert.match(get('museSaved').textContent,/Token 已保存/);assert.equal(get('museProxyPort').value,7897);
 assert.equal(get('retryMuse').hidden,false);assert.equal(posts().length,savedWrites,'read failure never clears configuration');
 // A cold failed read cannot turn blank fields into a request to clear a saved proxy.
 vm.runInContext('museConfigured=false;museLoaded=false;museProxyEdited=false',context);
 get('museProxyHost').value='';get('museProxyPort').value='';await context.loadMuse();await get('saveMuse').onclick();
 assert.equal(posts().length,savedWrites);assert.match(get('museStatus').textContent,/重新读取/);
 failMuseLoad=false;await get('retryMuse').onclick();
 assert.equal(get('museProxyHost').value,'192.168.1.20');assert.equal(get('museProxyPort').value,7897);
 assert.equal(get('retryMuse').hidden,true);
 // A successful POST supplies persisted state, without needing another fragile GET.
 get('museProxyPort').value='8080';failMuseLoad=true;
 const readsBeforeSave=requests.filter(r=>r.url.startsWith('/api/muse')&&!r.options.method).length;
 await get('saveMuse').onclick();
 assert.match(get('museSaved').textContent,/192\.168\.1\.20:8080/);assert.match(get('museStatus').textContent,/已保存/);
 assert.equal(requests.filter(r=>r.url.startsWith('/api/muse')&&!r.options.method).length,readsBeforeSave);
 failMuseLoad=false;
 get('museProxyHost').value='';await get('saveMuse').onclick();
 assert.deepEqual(JSON.parse(posts().at(-1).options.body),{proxy_host:'',proxy_port:0});
 assert.equal(musePaired,true,'proxy-only update does not replace account authorization');
 get('museProxyHost').value='';await get('saveMuse').onclick();
 // Primary completion saves valid pending Muse inputs, and never closes the
 // setup AP when those inputs are invalid or cannot be persisted.
 museTokenSet=false;musePaired=false;await context.loadMuse();
 let finishes=posts().filter(r=>r.url.startsWith('/api/finish')).length;
 get('museToken').value='wrong';await get('save').onclick();
 assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,finishes);
 assert.match(get('saveStatus').textContent,/SDK Token/);
 get('museToken').value='mgst_test_fixture_only_67890';failMuseSave=true;await get('save').onclick();
 assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,finishes,'failed Muse save keeps setup AP open');
 assert.equal(get('museToken').value,'mgst_test_fixture_only_67890','failed save preserves input');
 failMuseSave=false;let firstRequest=requests.length;await get('save').onclick();
 const completed=requests.slice(firstRequest).filter(r=>r.options.method==='POST').map(r=>r.url);
 assert.ok(completed.indexOf('/api/muse?t=314159')<completed.indexOf('/api/finish?t=314159'));
 assert.equal(get('museToken').value,'');assert.equal(get('step3').hidden,false);
 proxyPosts=posts().filter(r=>r.url.startsWith('/api/muse')).length;
 await get('save').onclick();
 assert.equal(posts().filter(r=>r.url.startsWith('/api/muse')).length,proxyPosts,'unchanged optional Muse configuration is not rewritten');
 // Both document-picker fallbacks use the same preview and binary save paths.
 get('fileFallback').files=[{size:1000}];await get('fileFallback').onchange();
 assert.equal(get('photoTools').hidden,false);assert.match(get('avatarStatus').textContent,/已选择/);
 get('shareFileFallback').files=[{size:1000}];await get('shareFileFallback').onchange();
 assert.equal(get('sharec').hidden,false);assert.match(get('shareStatus').textContent,/已选择/);
 const beforePickerPosts=posts().length;
 get('file').onclick();assert.match(get('avatarStatus').textContent,/正在打开选择器/);
 get('file').oncancel();assert.match(get('avatarStatus').textContent,/保留之前/);
 get('shareFileFallback').oncancel();assert.match(get('shareStatus').textContent,/保留之前/);
 get('fileFallback').files=[{size:0}];await get('fileFallback').onchange();assert.match(get('avatarStatus').textContent,/文件为空/);
 get('fileFallback').files=[{size:21*1024*1024}];await get('fileFallback').onchange();assert.match(get('avatarStatus').textContent,/超过 20 MB/);
 get('shareFileFallback').files=[{size:1000}];imageMode='error';await get('shareFileFallback').onchange();
 assert.match(get('shareStatus').textContent,/无法读取图片/);assert.equal(get('shareStatus').dataset.error,'true');
 imageMode='stall';get('fileFallback').files=[{size:1000}];
 const pendingImage=get('fileFallback').onchange();await get('save').onclick();
 assert.equal(posts().length,beforePickerPosts,'unreadable/loading images must not send settings or finish');
 assert.match(get('saveStatus').textContent,/图片还在读取/);
 const imageTimeout=[...timers.values()].find(t=>t.ms===15000);assert.ok(imageTimeout);imageTimeout.fn();await pendingImage;
 assert.match(get('avatarStatus').textContent,/读取图片超时/);
 imageMode='ok';await get('fileFallback').onchange();
 assert.match(get('avatarStatus').textContent,/已选择/,'retry recovers after decode timeout');
 await get('save').onclick();assert.equal(get('step3').hidden,false);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/avatar')).at(-1).options.body.byteLength,4976);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/share')).at(-1).options.body.type,'image/jpeg');
 // Nationwide search and stable ids distinguish repeated county names.
 assert.ok(cityCatalog.length>=3000);assert.equal(new Set(cityCatalog.map(c=>c[4])).size,34);
 get('province').value='广东';get('province').onchange();
 get('citySearch').value='珠海';get('citySearch').oninput();
 const zhuhai=cityCatalog.find(c=>c[1]==='珠海'&&c[4]==='广东');
 assert.ok(get('city').options.some(o=>o.value===zhuhai[0]));
 get('city').value=zhuhai[0];get('city').onchange();assert.equal(get('customCityFields').hidden,true);
 await get('save').onclick();profile=JSON.parse(posts().filter(r=>r.url.startsWith('/api/profile')).at(-1).options.body);
 assert.deepEqual({city:profile.city,region:profile.region,radio_city:profile.radio_city,country_code:profile.country_code,lat:profile.lat,lon:profile.lon},
     {city:'珠海',region:'广东',radio_city:'珠海',country_code:'CN',lat:'22.2707',lon:'113.5767'});
 get('citySearch').value='不存在的地点';get('citySearch').oninput();
 assert.match(get('cityStatus').textContent,/没有找到/);assert.equal(get('city').value,zhuhai[0],'search does not erase the saved choice');
 const chaoyang=cityCatalog.filter(c=>c[1]==='朝阳');assert.ok(chaoyang.length>1);
 assert.equal(new Set(chaoyang.map(c=>c[0])).size,chaoyang.length);
 get('city').value='__custom';get('city').onchange();get('customCity').value='测试新区';get('customLat').value='91';get('customLon').value='113.6';
 let beforeInvalid=posts().length;await get('save').onclick();assert.equal(posts().length,beforeInvalid);assert.match(get('saveStatus').textContent,/有效/);
 get('customLat').value='';get('customLon').value='';await get('save').onclick();
 profile=JSON.parse(posts().filter(r=>r.url.startsWith('/api/profile')).at(-1).options.body);assert.equal(profile.city,'测试新区');assert.equal(profile.lat,'');
 get('city').value='';get('city').onchange();await get('save').onclick();
 profile=JSON.parse(posts().filter(r=>r.url.startsWith('/api/profile')).at(-1).options.body);assert.equal(profile.city,'');assert.equal(profile.region,'');
 // Cold directory failure must not turn a saved catalogue city into a custom one.
 const savedCity={city:'珠海',region:'广东',radio_city:'珠海',country_code:'CN',lat:'22.2707',lon:'113.5767'};
 savedProfile={...savedProfile,...savedCity};
 vm.runInContext('cities=[]',context);failCities=true;await context.load();
 assert.match(get('cityStatus').textContent,/目录未能读取/);
 assert.equal(get('city').value,'__saved');assert.match(get('city').selectedOptions[0].text,/珠海/);
 assert.equal(get('customCityFields').hidden,true);assert.equal(get('retryCities').hidden,false);
 assert.deepEqual(JSON.parse(JSON.stringify(context.selectedCityProfile())),savedCity);
 get('province').value='北京';get('province').onchange();get('citySearch').value='无匹配';get('citySearch').oninput();
 assert.equal(get('city').value,'__saved','filters preserve the saved fallback');
 for(let i=0;i<2;i++)await get('retryCities').onclick();
 assert.equal(get('city').options.filter(o=>o.value==='__saved').length,1,'failed retries do not duplicate saved options');
 assert.equal(get('retryCities').disabled,false);
 await get('save').onclick();profile=JSON.parse(posts().filter(r=>r.url.startsWith('/api/profile')).at(-1).options.body);
 for(const key of Object.keys(savedCity))assert.equal(profile[key],savedCity[key],'offline save preserves '+key);
 failCities=false;await get('retryCities').onclick();
 assert.equal(get('city').value,zhuhai[0]);assert.equal(get('customCityFields').hidden,true);assert.equal(get('retryCities').hidden,true);
 assert.match(get('cityStatus').textContent,new RegExp(String(cityCatalog.length)));
 failCities=true;await context.load();get('city').value='__custom';get('city').onchange();
 get('customCity').value='手动新区';get('customLat').value='22.4';get('customLon').value='113.7';
 failCities=false;await get('retryCities').onclick();
 assert.equal(get('city').value,'__custom');assert.equal(get('customCity').value,'手动新区');assert.equal(get('customLat').value,'22.4');
 failCities=true;vm.runInContext('cities=[]',context);await context.loadCities();context.restoreCity({});
 assert.equal(get('city').value,'');assert.equal(get('customCityFields').hidden,true,'first boot does not fabricate a saved city');
 context.restoreCity(savedCity);failCities=false;let releaseCity;
 cityGate=new Promise(resolve=>releaseCity=resolve);const pendingCityRetry=get('retryCities').onclick();
 get('city').value='__custom';get('city').onchange();get('customCity').value='重试期间修改';
 releaseCity();await pendingCityRetry;cityGate=null;
 assert.equal(get('city').value,'__custom');assert.equal(get('customCity').value,'重试期间修改','in-flight retry does not overwrite deliberate edits');
 failCities=false;await context.load();
 context.restoreCity({city:'珠海',region:'广东',lat:'22.5',lon:'113.8'});
 assert.equal(get('city').value,'__custom');assert.equal(get('customLat').value,'22.5','custom coordinates for an existing name remain custom');
 // Phone coordinates reach weather unchanged; nearest place is only a label for radio.
 const target=cityCatalog.find(c=>c[1]==='香洲'&&c[4]==='广东');assert.ok(target);
 const lat=target[2]+.001,lon=target[3]+.001;
 let callbacks=null;phoneWindow.isSecureContext=true;
 phoneNavigator.geolocation={getCurrentPosition(ok,fail,options){callbacks={ok,fail};assert.equal(options.timeout,20000);assert.equal(options.enableHighAccuracy,true)}};
 get('locate').onclick();assert.equal(get('locate').disabled,true);callbacks.ok({coords:{latitude:lat,longitude:lon,accuracy:12}});
 assert.equal(get('city').value,target[0]);assert.match(get('locationStatus').textContent,/附近地点/);
 profile=context.selectedCityProfile();assert.equal(profile.lat,lat.toFixed(6));assert.equal(profile.lon,lon.toFixed(6));assert.equal(profile.radio_city,'珠海');
 await get('save').onclick();profile=JSON.parse(posts().filter(r=>r.url.startsWith('/api/profile')).at(-1).options.body);assert.equal(profile.lat,lat.toFixed(6));
 // Restoring a nearby location must never snap actual phone coordinates to the city centre.
 context.restoreCity({...profile});assert.equal(context.selectedCityProfile().lat,lat.toFixed(6));
 get('locate').onclick();callbacks.fail({code:1});assert.match(get('locationStatus').textContent,/权限/);assert.equal(context.selectedCityProfile().lat,lat.toFixed(6));
 get('locate').onclick();const stale=callbacks;get('city').value=zhuhai[0];get('city').onchange();stale.ok({coords:{latitude:lat,longitude:lon,accuracy:12}});assert.equal(get('city').value,zhuhai[0]);
 get('locate').onclick();callbacks.fail({code:3});assert.match(get('locationStatus').textContent,/超时/);assert.equal(get('locate').disabled,false);
 assert.throws(()=>context.applyPhonePosition(91,lon,2),/无效/);assert.throws(()=>context.applyPhonePosition(lat,lon,60000),/范围太大/);assert.throws(()=>context.applyPhonePosition(0,0,2),/没有目录地点/);
 phoneWindow.isSecureContext=false;get('locate').onclick();assert.match(get('locationStatus').textContent,/入口尚未启用/);
 // Real HTTP hotspot uses the HTTPS helper: origin + popup + nonce all must match.
 get('locate').dataset.helper='https://location.example/phone-location.html';const popup={};let opened='';phoneWindow.open=url=>{opened=url;return popup};
 get('locate').onclick();const helper=new URL(opened),params=new URLSearchParams(helper.hash.slice(1));assert.equal(params.get('origin'),'http://192.168.4.1');assert.ok(!opened.includes('314159'),'device token never leaves the hotspot');
 const data={type:'passport-position',request:params.get('request'),lat,lon,accuracy:12};
 context.receiveLocation({source:popup,origin:'https://wrong.example',data});assert.equal(get('city').value,zhuhai[0]);
 context.receiveLocation({source:{},origin:helper.origin,data});assert.equal(get('city').value,zhuhai[0]);
 context.receiveLocation({source:popup,origin:helper.origin,data:{...data,request:'0'.repeat(32)}});assert.equal(get('city').value,zhuhai[0]);
 context.receiveLocation({source:popup,origin:helper.origin,data});assert.equal(get('city').value,target[0]);assert.equal(get('locate').disabled,false);
 get('city').value=zhuhai[0];get('city').onchange();context.receiveLocation({source:popup,origin:helper.origin,data});assert.equal(get('city').value,zhuhai[0],'late/replayed helper result is ignored');
 get('locationCode').value='PPOS:'+lat.toFixed(6)+','+lon.toFixed(6)+',12';get('importLocation').onclick();assert.equal(get('city').value,target[0]);
 get('locationCode').value='PPOS:91,113,10';get('importLocation').onclick();assert.match(get('locationStatus').textContent,/无效/);
 phoneWindow.open=()=>null;get('locate').onclick();assert.match(get('locationStatus').textContent,/新窗口/);
 get('locate').dataset.helper='http://location.example';get('locate').onclick();assert.match(get('locationStatus').textContent,/入口尚未启用/);
 // Real gallery state: compressed blobs only, stable slots, order/delete and retry.
 await context.loadCards();
 get('shareFile').files=Array.from({length:6-gallery.length},()=>({size:1000}));await get('shareFile').onchange();
 assert.equal(vm.runInContext('cards.length',context),6);
 assert.equal(vm.runInContext('shareImg',context),null,'original bitmap released after compression');
 get('shareFile').files=[{size:1000}];await get('shareFile').onchange();assert.match(get('shareStatus').textContent,/最多保存 6/);
 await context.saveCards();assert.equal(gallery.length,6);
 const original=gallery.map(c=>c.slot);context.moveCard(5,-1);await context.saveCards();assert.equal(gallery[4].slot,original[5]);
 context.removeCard(0);await context.saveCards();assert.equal(gallery.length,5);
 const replacing=gallery[0].slot;context.chooseReplacement(replacing);get('shareReplace').files=[{size:1000}];await get('shareReplace').onchange();
 const beforeRetry=posts().filter(r=>r.url.startsWith('/api/share')).length;failShare=true;await get('save').onclick();
 assert.equal(vm.runInContext('cards.find(c=>c.slot===cards[0].slot).blob.size',context),20000,'failed upload retains compressed retry data');
 failShare=false;await get('save').onclick();assert.equal(gallery.length,5);assert.equal(gallery[0].slot,replacing);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/share')).length,beforeRetry+2);
 qualityAttempts=[];blobBytes=q=>q>.48?30000:24000;get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 assert.deepEqual(qualityAttempts,[.86,.78,.68,.58,.48]);assert.equal(vm.runInContext('cards.at(-1).bytes',context),24000);
 context.removeCard(5);blobBytes=30000;qualityAttempts=[];get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 assert.match(get('shareStatus').textContent,/压缩后仍较大/);assert.equal(vm.runInContext('cards.length',context),5);
 assert.equal(qualityAttempts.length,7);
 // Native chooser contracts: batch inputs stay multiple; replacement is independent.
const batchTag=html.match(/<input[^>]*id="shareFile"[^>]*>/)[0];
assert.match(batchTag,/\bmultiple\b/);assert.ok(!/\baccept=|\bcapture=/.test(batchTag));
assert.match(html.match(/<input[^>]*id="shareFileFallback"[^>]*>/)[0],/\bmultiple\b/);
assert.ok(!/\bmultiple\b/.test(html.match(/<input[^>]*id="shareReplace"[^>]*>/)[0]));
// Starting batch addition after replacement selection/cancel must never overwrite it.
blobBytes=20000;await context.loadCards();context.chooseReplacement(gallery[0].slot);get('shareReplace').oncancel();
get('shareFile').onclick();get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(vm.runInContext('cards.length',context),6,'batch adds a card instead of replacing an old one');
context.removeCard(5);get('shareFile').files=[{size:1000,type:'application/pdf'}];await get('shareFile').onchange();
assert.match(get('shareStatus').textContent,/不支持文档或视频/);assert.equal(vm.runInContext('cards.length',context),5);
get('shareFileFallback').onclick();get('shareFileFallback').files=[{size:1000,type:'image/png'}];await get('shareFileFallback').onchange();
assert.equal(vm.runInContext('cards.length',context),6);
// A selected filename remains visible after processing; retry resets it on click.
context.removeCard(5);
get('shareFile').value='C:\\fakepath\\card.png';
get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(get('shareFile').value,'C:\\fakepath\\card.png');
context.removeCard(5);get('shareFile').onclick();assert.equal(get('shareFile').value,'');
get('shareFile').files=[];await get('shareFile').onchange();
assert.match(get('shareStatus').textContent,/保留之前/);
// Reading and encoding are separate stages; stalled encoding can be retried.
const originalToBlob=get('sharec').toBlob;let lateEncode;
get('sharec').toBlob=cb=>{lateEncode=cb};
get('shareFile').value='C:\\fakepath\\card.png';get('shareFile').files=[{size:1000,type:'image/png'}];
const stalledEncoding=get('shareFile').onchange();await flush();await flush();
assert.match(get('shareStatus').textContent,/正在压缩/);assert.equal(vm.runInContext('cardBusy',context),true);
const encodingTimeout=[...timers.values()].find(t=>t.ms===10000);assert.ok(encodingTimeout);
encodingTimeout.fn();await stalledEncoding;
assert.match(get('shareStatus').textContent,/图片压缩超时/);
assert.equal(vm.runInContext('cardBusy',context),false);assert.equal(vm.runInContext('shareImg',context),null);
assert.equal(get('shareFile').value,'C:\\fakepath\\card.png','timeout must not erase the selected filename');
assert.equal(vm.runInContext('cards.length',context),5);
lateEncode(new Blob([new Uint8Array(1000)],{type:'image/jpeg'}));await flush();
assert.equal(vm.runInContext('cards.length',context),5,'late encoding callback cannot add a card');
get('sharec').toBlob=originalToBlob;get('shareFile').onclick();
get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(vm.runInContext('cards.length',context),6,'same file can be retried after encoding timeout');
context.removeCard(5);
// Reject null, unexpected MIME and thrown encoding errors without wedging the picker.
for(const encode of [cb=>cb(null),cb=>cb(new Blob(['png'],{type:'image/png'})),()=>{throw new Error('canvas unavailable')}]){
 get('sharec').toBlob=encode;await get('shareFile').onchange();
 assert.equal(get('shareStatus').dataset.error,'true');assert.equal(vm.runInContext('cardBusy',context),false);
 assert.equal(vm.runInContext('cards.length',context),5);
}
get('sharec').toBlob=originalToBlob;imageMode='stall';
const stalledReading=get('shareFile').onchange();await flush();
assert.match(get('shareStatus').textContent,/正在读取/);
const readingTimeout=[...timers.values()].find(t=>t.ms===15000);assert.ok(readingTimeout);
readingTimeout.fn();await stalledReading;imageMode='ok';
assert.match(get('shareStatus').textContent,/读取图片超时/);assert.equal(vm.runInContext('cardBusy',context),false);
assert.ok(html.indexOf('id="shareStatus"')<html.indexOf('id="cardList"'),'picker feedback precedes the gallery');
console.log('Image feedback: filename preservation, distinct decode/encode stages, encoding timeout/retry/late callback, MIME/null/exception recovery: PASS');
console.log('Multiselect: unfiltered multiple file input, multiple photo alternative, separate single replacement, cancelled replacement isolation and non-image rejection: PASS');
 console.log('Card gallery: six-image limit, sequential compression, released original bitmaps, quality ladder, reject oversized, stable slots, reorder/delete and partial-save retry: PASS');
 console.log('Phone setup: Wi-Fi/images/Muse, nationwide cities, phone GPS/raw coordinates, permission/timeout/manual override, HTTPS popup authentication and copy fallback: PASS');
})().catch(e=>{console.error(e);process.exitCode=1});
