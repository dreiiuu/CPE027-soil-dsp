// dashboard.h - single-page web dashboard served by the ESP32-S3.
// Polls /api/state once per second, plots the cross-sensor median (before smoothing) and the
// smoothed output on the same axes with the thresholds, and can download the session as the
// same CSV the serial log uses (t_s,s1,s2,s3,pump) so analysis/analyze_log.py accepts it.
#pragma once
#include <pgmspace.h>

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Soil DSP Dashboard</title>
<style>
:root{--bg:#10150f;--card:#1a211a;--fg:#e8efe4;--mut:#93a08e;--g:#7fbf6a;--o:#e0a040;--b:#6fa8dc;--r:#e06c5a;--grid:#2a352a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif;padding:12px;max-width:980px;margin:auto}
h1{font-size:18px;margin:4px 0 10px}.row{display:flex;flex-wrap:wrap;gap:10px}
.card{background:var(--card);border:1px solid var(--grid);border-radius:10px;padding:10px 12px;flex:1 1 150px}
.k{color:var(--mut);font-size:12px}.v{font-size:22px;font-weight:700}
#status{font-size:34px;letter-spacing:1px}
canvas{width:100%;height:280px;background:var(--card);border:1px solid var(--grid);border-radius:10px;display:block;margin:10px 0}
button{background:#263326;color:var(--fg);border:1px solid #3a4a3a;border-radius:8px;padding:8px 10px;margin:3px 3px 3px 0;cursor:pointer}
button:hover{background:#2f402f}table{border-collapse:collapse;width:100%}td,th{padding:3px 6px;border-bottom:1px solid var(--grid);text-align:right}th:first-child,td:first-child{text-align:left}
.leg span{margin-right:12px;font-size:12px}.sw{display:inline-block;width:12px;height:3px;vertical-align:middle;margin-right:4px}
small{color:var(--mut)}input{width:64px;background:#0d120d;color:var(--fg);border:1px solid #3a4a3a;border-radius:6px;padding:4px}
</style></head><body>
<h1>Soil moisture DSP dashboard <small id="net"></small></h1>
<div class="row">
<div class="card"><div class="k">Status (smoothed)</div><div class="v" id="status">--</div><small id="calflag"></small></div>
<div class="card"><div class="k">Smoothed index y[n]</div><div class="v" id="y">--</div></div>
<div class="card"><div class="k">Median x[n] (unsmoothed)</div><div class="v" id="x">--</div></div>
<div class="card"><div class="k">Pump</div><div class="v" id="pump">--</div><small id="en"></small></div>
</div>
<canvas id="c" width="960" height="280"></canvas>
<div class="leg"><span><i class="sw" style="background:#93a08e"></i>x[n] cross-sensor median</span>
<span><i class="sw" style="background:var(--g)"></i>y[n] median+EMA (proposed)</span>
<span><i class="sw" style="background:var(--o)"></i>moving average (M=10)</span>
<span><i class="sw" style="background:var(--r)"></i>&theta;dry</span><span><i class="sw" style="background:var(--b)"></i>&theta;wet</span></div>
<div class="row" style="margin-top:10px">
<div class="card"><div class="k">Sensors (calibrated index)</div>
<table><tr><th></th><th>Pin</th><th>Raw ADC</th><th>Index</th><th>Wiring</th></tr>
<tr><td>S1 (v1.2)</td><td id="g0">-</td><td id="a0">-</td><td id="m0">-</td><td id="f0">-</td></tr>
<tr><td>S2 (v2.0)</td><td id="g1">-</td><td id="a1">-</td><td id="m1">-</td><td id="f1">-</td></tr>
<tr><td>S3 (v2.0)</td><td id="g2">-</td><td id="a2">-</td><td id="m2">-</td><td id="f2">-</td></tr></table>
<small id="relaypin"></small></div>
<div class="card"><div class="k">Decision toggles at &theta;dry (since reset)</div>
<table><tr><td>Raw sensor 1</td><td id="t0">-</td></tr><tr><td>Cross-sensor median</td><td id="t1">-</td></tr>
<tr><td>Moving average</td><td id="t2">-</td></tr><tr><td>Median + EMA</td><td id="t3">-</td></tr></table>
<button onclick="api('/api/reset_toggles')">Reset counters</button></div>
</div>
<div class="card" style="margin-top:10px">
<div class="k">Controls</div>
<button onclick="api('/api/pump?en='+(S.en?0:1))" id="bEn">Pump control</button>
<button onclick="api('/api/blank?en='+(S.blank?0:1))" id="bBl">Blanking</button>
<button onclick="api('/api/test?on='+(S.test?0:1))" id="bTe">Pump test cycle</button>
<button onclick="if(confirm('Probes in AIR (dry)? Captures 60 s.'))api('/api/cal?type=dry')">Calibrate DRY (air)</button>
<button onclick="if(confirm('Probes in WATER up to the line? Captures 60 s.'))api('/api/cal?type=wet')">Calibrate WET (water)</button>
<button onclick="dl()">Download CSV</button><button onclick="clr()">Clear plot</button>
<div id="calst" style="margin-top:6px"></div>
<div style="margin-top:8px"><small>Thresholds (index): </small>
dry <input id="pd"> hyst <input id="ph"> wet <input id="pw"> Tmax s <input id="pt"> lockout s <input id="pl"> blank s <input id="pb">
<button onclick="sp()">Save</button></div></div>
<script>
let S={},H={t:[],a:[],x:[],y:[],ma:[],p:[]},MAXV=900;
const $=id=>document.getElementById(id);
async function api(u){try{await fetch(u)}catch(e){}}
function push(s){H.t.push(s.t);H.a.push(s.a);H.x.push(s.x);H.y.push(s.y);H.ma.push(s.ma);H.p.push(s.pump)}
function clr(){for(const k in H)H[k]=[];draw()}
function dl(){let r=["t_s,s1,s2,s3,pump"];for(let i=0;i<H.t.length;i++)r.push([H.t[i],...H.a[i],H.p[i]].join(","));
const b=new Blob([r.join("\n")+"\n"],{type:"text/csv"}),a=document.createElement("a");a.href=URL.createObjectURL(b);a.download="soil_log.csv";a.click()}
function sp(){api('/api/params?thdry='+$('pd').value+'&h='+$('ph').value+'&thwet='+$('pw').value+'&tmax='+$('pt').value+'&lock='+$('pl').value+'&blank='+$('pb').value)}
let filled=false;
function draw(){const c=$('c'),dpr=window.devicePixelRatio||1,cw=c.clientWidth||960,ch=c.clientHeight||280;
if(c.width!==Math.round(cw*dpr)||c.height!==Math.round(ch*dpr)){c.width=Math.round(cw*dpr);c.height=Math.round(ch*dpr)}
const g=c.getContext('2d');g.setTransform(dpr,0,0,dpr,0,0);const W=cw,Hh=ch,L=34,B=20,T=8,R=8;g.clearRect(0,0,W,Hh);
// y range follows the data (so the fluctuation is visible); a threshold is drawn only when it is near the data
const n=H.t.length;let lo=0,hi=100;if(n){const all=H.x.concat(H.y,H.ma);const mn=Math.min(...all),mx=Math.max(...all);
lo=Math.max(0,Math.floor(mn-3));hi=Math.min(100,Math.ceil(mx+3));
if(S.thdry!=null&&S.thdry>=mn-15&&S.thdry<=mx+15){lo=Math.min(lo,Math.floor(S.thdry-2));hi=Math.max(hi,Math.ceil(S.thdry+2))}
if(S.thwet!=null&&S.thwet>=mn-15&&S.thwet<=mx+15){lo=Math.min(lo,Math.floor(S.thwet-2));hi=Math.max(hi,Math.ceil(S.thwet+2))}
lo=Math.max(0,lo);hi=Math.min(100,hi);if(hi-lo<6){const m=(hi+lo)/2;lo=Math.max(0,m-3);hi=lo+6}}
const X=i=>L+(W-L-R)*(n>1?i/(n-1):0),Y=v=>T+(Hh-T-B)*(1-(v-lo)/(hi-lo));
g.strokeStyle='#2a352a';g.fillStyle='#93a08e';g.font='11px system-ui';g.lineWidth=1;
for(let k=0;k<=4;k++){const v=lo+(hi-lo)*k/4,y=Y(v);g.beginPath();g.moveTo(L,y);g.lineTo(W-R,y);g.stroke();g.fillText(v.toFixed(0),4,y+4)}
function line(a,col,w){g.strokeStyle=col;g.lineWidth=w;g.beginPath();a.forEach((v,i)=>{i?g.lineTo(X(i),Y(v)):g.moveTo(X(i),Y(v))});g.stroke()}
const off=[];function hl(v,col,name){if(v==null)return;if(v<lo||v>hi){off.push(name+' '+v+(v>hi?' (above)':' (below)'));return}
g.strokeStyle=col;g.setLineDash([6,4]);g.lineWidth=1.2;g.beginPath();g.moveTo(L,Y(v));g.lineTo(W-R,Y(v));g.stroke();g.setLineDash([])}
hl(S.thdry,'#e06c5a','θdry');hl(S.thwet,'#6fa8dc','θwet');
if(n){line(H.x,'#93a08e',1);line(H.ma,'#e0a040',1.4);line(H.y,'#7fbf6a',2.2);
g.fillStyle='#93a08e';g.fillText('t = '+H.t[0]+' ... '+H.t[n-1]+' s',L,Hh-4);
if(off.length){g.textAlign='right';g.fillText(off.join('   '),W-R,T+10);g.textAlign='left'}}}
window.addEventListener('resize',draw);
function upd(s){S=s;push(s);if(H.t.length>MAXV)for(const k in H)H[k].shift();
const col={DRY:'#e06c5a',OK:'#7fbf6a',WET:'#6fa8dc'}[s.status];$('status').textContent=s.status;$('status').style.color=col;
$('y').textContent=s.y.toFixed(1);$('x').textContent=s.x.toFixed(1);
$('pump').textContent=s.pump?'ON':'OFF';$('en').textContent=s.test?'test cycle running':(s.en?'control enabled':'control disabled');
const fl=s.flt||[0,0,0],FN=['OK','NO SIGNAL','AT 3.3V'];
for(let i=0;i<3;i++){$('a'+i).textContent=s.a[i];$('m'+i).textContent=s.m[i].toFixed(1);
$('f'+i).textContent=FN[fl[i]]||'?';$('f'+i).style.color=fl[i]?'#e0a040':'';if(s.pins)$('g'+i).textContent='GPIO'+s.pins.s[i]}
if(s.pins)$('relaypin').textContent='Relay IN: GPIO'+s.pins.relay+' (active '+(s.pins.relayActiveLow?'LOW':'HIGH')+')';
for(let i=0;i<4;i++)$('t'+i).textContent=s.tog[i];
$('calflag').textContent=s.calok?'':'UNCALIBRATED - pump locked';
$('bEn').textContent='Pump control: '+(s.en?'ON':'OFF');$('bBl').textContent='Blanking: '+(s.blank?'ON':'OFF');$('bTe').textContent='Pump test cycle: '+(s.test?'ON':'OFF');
$('calst').textContent=s.cal.busy?('Calibrating '+s.cal.type+': '+s.cal.n+'/60 s - keep the probes still'):'';
$('net').textContent=s.ip;
if(!filled||document.activeElement.tagName!=='INPUT'){if(!filled){$('pd').value=s.thdry;$('ph').value=s.h;$('pw').value=s.thwet;$('pt').value=s.tmax;$('pl').value=s.lock;$('pb').value=s.blankS;filled=true}}
draw()}
async function init(){try{const h=await (await fetch('/api/history')).json();h.t.forEach((t,i)=>push({t:t,a:h.a[i],x:h.x[i],y:h.y[i],ma:h.ma[i],pump:h.p[i]}))}catch(e){}}
async function poll(){try{const s=await (await fetch('/api/state')).json();if(!H.t.length||H.t[H.t.length-1]!==s.t)upd(s);else S=s}catch(e){$('net').textContent='offline'}}
init().then(()=>{poll();setInterval(poll,1000)});
</script></body></html>
)HTML";
