// Setup page served by the keyring in setup mode (http://192.168.4.1)
// English / Korean, switchable at the top of the page.
#pragma once
const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Santa Tracker Setup</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:-apple-system,system-ui,sans-serif;background:#0B1530;color:#F4F7FB;word-break:keep-all}
main{max-width:440px;margin:0 auto;padding:16px 16px 40px}
.top{display:flex;justify-content:flex-end}
.lang{display:flex;border:1px solid #2C3D72;border-radius:999px;overflow:hidden}
.lang button{width:auto;margin:0;padding:6px 12px;background:transparent;color:#9FB0D0;font-size:13px;font-weight:600;border-radius:0}
.lang button.on{background:#F5C451;color:#0B1530}
h1{font-size:22px;margin:8px 0 2px}
.sub{color:#9FB0D0;font-size:13px;margin:0 0 18px}
section{background:#16234A;border-radius:12px;padding:14px;margin:0 0 14px}
h2{font-size:15px;margin:0 0 10px;color:#F5C451}
label{display:block;font-size:13px;color:#9FB0D0;margin:10px 0 4px}
input,select{width:100%;padding:11px 10px;border-radius:8px;border:1px solid #2C3D72;background:#0B1530;color:#F4F7FB;font-size:16px}
.row{display:flex;gap:8px}.row>*{flex:1}
.hint{font-size:12px;color:#9FB0D0;margin-top:8px;line-height:1.45}
button{width:100%;padding:14px;border:0;border-radius:10px;background:#E5484D;color:#fff;font-size:17px;font-weight:700;margin-top:6px}
button.ghost{background:transparent;border:1px solid #2C3D72;color:#9FB0D0;font-size:14px;padding:10px;font-weight:400}
</style></head><body><main>
<div class="top"><div class="lang"><button type="button" id="L_en" onclick="setLang('en')">EN</button><button type="button" id="L_ko" onclick="setLang('ko')">한국어</button></div></div>
<h1>🎅 <span data-t="title"></span></h1>
<p class="sub" data-t="sub"></p>
<form id="f" method="post" action="/save">
<input type="hidden" name="lang" id="lang" value="en">
<section><h2 data-t="w1"></h2>
<label data-t="name"></label><input name="s1" id="s1" list="nets" autocomplete="off">
<label data-t="pw"></label><input name="p1" id="p1" type="password">
</section>
<section><h2 data-t="w2"></h2>
<label data-t="name"></label><input name="s2" id="s2" list="nets" autocomplete="off">
<label data-t="pw"></label><input name="p2" id="p2" type="password">
<p class="hint" data-t="whint"></p>
<button type="button" class="ghost" onclick="scan()" data-t="rescan"></button>
</section>
<datalist id="nets"></datalist>
<section><h2 data-t="home"></h2>
<label data-t="search"></label><input id="q" autocomplete="off">
<label data-t="city"></label><select name="city" id="city"></select>
<p class="hint" id="cinfo"></p>
</section>
<section><h2 data-t="screen"></h2>
<div class="row"><div><label data-t="ontime"></label><select name="on" id="on"></select></div>
<div><label data-t="alert"></label><select name="al" id="al"></select></div></div>
<p class="hint" data-t="bhint"></p>
</section>
<section><h2 data-t="demo"></h2>
<select name="demo" id="demo"></select>
<p class="hint" data-t="dhint"></p>
</section>
<button type="submit" data-t="save"></button>
</form></main>
<script>
const T={
en:{title:'Santa Tracker Setup',sub:'Choose your WiFi and your home city.',
w1:'WiFi 1 (phone hotspot recommended)',w2:'WiFi 2 (optional, e.g. home WiFi)',name:'Network name',pw:'Password',
whint:'Leave a password empty to keep the saved one.<br>2.4 GHz WiFi only. WiFi 1 is tried first, then WiFi 2. WiFi is only used briefly to set the clock.',
rescan:'Scan for networks again',home:'Home city',search:'Search',ph:'e.g. seoul, london, new york',city:'City',
arr:'Santa arrives: Dec {d}, {t} local time ({z})',screen:'Screen',ontime:'Screen on time',alert:'"Santa is near" alert',
never:'Never off',sec:'sec',min:'min',hr:'h',before:'before',hour:'1 hour',
bhint:'Battery estimates: 300 mAh battery, about 10 looks a day. This board can\'t switch its backlight off, so even asleep the keyring uses some power &mdash; that\'s why the times are short.',
demo:'Demo mode',d0:'Off — normal use',d1:'Full demo (developer mode) — real-time night from takeoff',d2:'Tap demo — each tap switches the screen',
dhint:'<b>For developers and testing — if you just want to use the keyring, choose Off.</b><br>Full demo (developer mode): sets the clock to 1 minute before takeoff on Dec 24 and then runs at real speed, so you can test the whole night exactly as it will happen (sleep, taps, alert and arrival wake-ups).<br>Tap demo: tap the back to switch screens — ready → live → map → alert → arrived → left. The screen stays on.<br>To leave a demo, tap 4 times and choose Off.',
save:'Save &amp; restart'},
ko:{title:'산타 트래커 설정',sub:'와이파이와 사는 도시를 정해 주세요.',
w1:'와이파이 1 (휴대폰 핫스팟 추천)',w2:'와이파이 2 (선택, 집 와이파이 등)',name:'이름',pw:'비밀번호',
whint:'이미 저장한 비밀번호는 비워 두면 그대로 유지돼요.<br>2.4GHz 와이파이만 돼요. 1번을 먼저 시도하고, 안 되면 2번으로 연결해요. 와이파이는 시간을 맞출 때만 잠깐 써요.',
rescan:'주변 와이파이 다시 찾기',home:'우리 동네',search:'도시 검색 (영어)',ph:'예: seoul, busan, tokyo',city:'도시',
arr:'산타 도착 예정: 12/{d} {t} (현지 시간, {z})',screen:'화면',ontime:'화면 켜짐 시간',alert:'산타 근접 알림',
never:'꺼지지 않음',sec:'초',min:'분',hr:'시간',before:'전',hour:'1시간',
bhint:'예상 사용시간: 300mAh 배터리, 하루 10번 확인 기준이에요. 이 보드는 백라이트를 끌 수 없어서 잠든 동안에도 전기를 조금씩 써요. 그래서 사용시간이 짧아요.',
demo:'데모 모드',d0:'끄기 — 일반 사용',d1:'전체 데모 (개발자 모드) — 출발부터 실제 시간 그대로',d2:'터치 데모 — 톡 칠 때마다 화면 전환',
dhint:'<b>개발자·테스트용 기능이에요. 일반 사용자는 끄기를 선택하면 됩니다.</b><br>전체 데모(개발자 모드): 시계를 12/24 출발 1분 전으로 맞춘 뒤 실제 속도로 흘러가요. 화면 꺼짐, 터치, 알림·도착 때 자동으로 깨어나기까지 실제 크리스마스이브와 똑같이 테스트할 수 있어요.<br>터치 데모: 뒷면을 톡 칠 때마다 준비중 → 비행중 → 지도 → 알림 → 도착 → 출발 순서로 화면이 바뀌어요. 화면은 꺼지지 않아요.<br>데모를 끝내려면 4번 톡톡 쳐서 끄기를 고르세요.',
save:'저장하고 다시 시작'}};
let L='en',C=[],D={};
const $=id=>document.getElementById(id);
const t=k=>T[L][k];
function tzs(m){const s=m<0?'-':'+';m=Math.abs(m);return 'UTC'+s+Math.floor(m/60)+(m%60?':'+String(m%60).padStart(2,'0'):'')}
// rough battery life: ~45 mA screen on, ~20 mA asleep (backlight stays on), 10 looks/day
function life(sec){if(!sec)return 300/45;return 300/(20+25*Math.min(1,10*sec/86400))}
const sp=()=>L=='ko'?'':' ';
function opts(id,list,cur){$(id).innerHTML=list.map(([v,txt])=>'<option value="'+v+'"'+(v==cur?' selected':'')+'>'+txt+'</option>').join('')}
function onOpts(cur){opts('on',[[0,t('never')],[30,'30'+sp()+t('sec')],[60,'1'+sp()+t('min')],[120,'2'+sp()+t('min')],[180,'3'+sp()+t('min')],[300,'5'+sp()+t('min')]]
 .map(([v,x])=>[v,x+' (~'+Math.round(life(v))+sp()+t('hr')+')']),cur)}
function alOpts(cur){opts('al',[[15,'15'+sp()+t('min')+' '+t('before')],[30,'30'+sp()+t('min')+' '+t('before')],[60,t('hour')+' '+t('before')]],cur)}
function demoOpts(cur){opts('demo',[[0,t('d0')],[1,t('d1')],[2,t('d2')]],cur)}
function setLang(l){L=l;$('lang').value=l;document.documentElement.lang=l;
 document.querySelectorAll('[data-t]').forEach(e=>e.innerHTML=t(e.dataset.t));
 $('q').placeholder=t('ph');$('L_en').className=l=='en'?'on':'';$('L_ko').className=l=='ko'?'on':'';
 onOpts($('on').value||D.on);alOpts($('al').value||D.al);demoOpts($('demo').value||D.demo);info()}
function fill(sel){const q=$('q').value.trim().toUpperCase();const cs=$('city');const cur=sel??cs.value;cs.innerHTML='';
 C.map((c,i)=>({c,i})).filter(o=>!q||o.c[0].includes(q)||o.c[1].includes(q))
 .sort((a,b)=>(a.c[1]+a.c[0]).localeCompare(b.c[1]+b.c[0]))
 .forEach(o=>{const op=document.createElement('option');op.value=o.c[0];op.textContent=o.c[1]+' · '+o.c[0];if(o.c[0]==cur)op.selected=true;cs.appendChild(op)});info()}
function info(){const v=$('city').value;const c=C.find(x=>x[0]==v);if(!c){$('cinfo').textContent='';return}
 const d=new Date(Date.UTC(2000,11,24,10,0,0)+c[2]*1000+c[3]*60000);
 $('cinfo').textContent=t('arr').replace('{d}',d.getUTCDate()).replace('{t}',String(d.getUTCHours()).padStart(2,'0')+':'+String(d.getUTCMinutes()).padStart(2,'0')).replace('{z}',tzs(c[3]))}
function nets(n){$('nets').innerHTML=n.map(s=>'<option value="'+s.replace(/"/g,'&quot;')+'">').join('')}
function scan(){fetch('/scan').then(r=>r.json()).then(nets)}
setLang('en');
fetch('/data').then(r=>r.json()).then(d=>{D=d;C=d.cities;$('s1').value=d.s1||'';$('s2').value=d.s2||'';
 onOpts(d.on);alOpts(d.al);demoOpts(d.demo);fill(d.city);nets(d.nets);setLang(d.lang||'en')});
$('q').oninput=()=>fill();$('city').onchange=info;
</script></body></html>)HTML";

const char SAVED_HTML[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Saved</title>
<style>body{margin:0;font-family:system-ui,sans-serif;background:#0B1530;color:#F4F7FB;text-align:center;padding:60px 20px}
p{color:#9FB0D0;line-height:1.5}</style></head><body><h1>🎄 Saved! / 저장했어요!</h1>
<p>The keyring is restarting.<br>This network (SantaTracker-Setup) will disappear in a moment.</p>
<p>키링이 다시 시작해요.<br>이 와이파이(SantaTracker-Setup)는 곧 사라져요.</p></body></html>)HTML";
