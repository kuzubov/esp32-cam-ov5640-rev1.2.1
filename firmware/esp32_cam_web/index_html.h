#pragma once
#include <Arduino.h>

// Web UI: live stream + all camera settings. Served from PROGMEM.
static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM OV5640</title>
<style>
:root{--bg:#111418;--panel:#1b2027;--line:#2a313b;--text:#e6e9ee;--muted:#8b95a3;--acc:#3b9eff;--bad:#ff5d5d;--ok:#3ccf7a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
header{display:flex;flex-wrap:wrap;gap:12px;align-items:center;justify-content:space-between;padding:10px 16px;border-bottom:1px solid var(--line)}
h1{font-size:16px;margin:0;font-weight:600}
.stats{display:flex;flex-wrap:wrap;gap:14px;color:var(--muted);font-size:12px;font-variant-numeric:tabular-nums}
.stats b{color:var(--text);font-weight:500}
main{display:grid;grid-template-columns:1fr 340px;gap:16px;padding:16px;align-items:start}
@media(max-width:900px){main{grid-template-columns:1fr}}
.view{background:#000;border:1px solid var(--line);border-radius:8px;overflow:hidden;position:relative;min-height:200px;display:flex;align-items:center;justify-content:center}
.view img{display:block;max-width:100%;height:auto}
.view .msg{position:absolute;color:var(--muted)}
.bar{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}
button{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:6px;padding:7px 12px;font:inherit;cursor:pointer}
button:hover{border-color:var(--acc)}
button.primary{background:var(--acc);border-color:var(--acc);color:#fff}
aside{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:4px 14px 14px}
details{border-bottom:1px solid var(--line)}
details:last-child{border-bottom:0}
summary{cursor:pointer;padding:10px 0;font-weight:600;font-size:13px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}
.row{display:grid;grid-template-columns:120px 1fr 34px;gap:8px;align-items:center;margin:7px 0}
.row label{color:var(--muted);font-size:13px}
.row .v{text-align:right;font-variant-numeric:tabular-nums;font-size:12px;color:var(--muted)}
.row.err label{color:var(--bad)}
input[type=range]{width:100%;accent-color:var(--acc)}
select{width:100%;background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:5px;padding:5px}
.sw{position:relative;width:38px;height:20px;justify-self:start}
.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:var(--line);border-radius:20px;transition:.15s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:14px;height:14px;left:3px;top:3px;background:#fff;border-radius:50%;transition:.15s}
.sw input:checked+span{background:var(--acc)}
.sw input:checked+span:before{transform:translateX(18px)}
.toast{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);background:var(--panel);border:1px solid var(--line);padding:8px 14px;border-radius:6px;opacity:0;transition:.2s;pointer-events:none}
.toast.show{opacity:1}
</style>
</head>
<body>
<header>
  <h1>ESP32-CAM · OV5640</h1>
  <div class="stats">
    <span>FPS <b id="s_fps">–</b></span>
    <span>Wi-Fi <b id="s_rssi">–</b></span>
    <span>IP <b id="s_ip">–</b></span>
    <span>Heap <b id="s_heap">–</b></span>
    <span>PSRAM <b id="s_psram">–</b></span>
    <span>SD <b id="s_sd">–</b></span>
    <span>Uptime <b id="s_up">–</b></span>
  </div>
</header>
<main>
  <section>
    <div class="view"><img id="stream" alt=""><span class="msg" id="msg">Stream stopped</span></div>
    <div class="bar">
      <button class="primary" id="b_stream">Start</button>
      <button id="b_still">Snapshot</button>
      <button id="b_save">Save frame</button>
      <button id="b_full">Fullscreen</button>
    </div>
  </section>
  <aside id="ctl"></aside>
</main>
<div class="toast" id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const base=location.origin, streamUrl=location.protocol+'//'+location.hostname+':81/stream';
// SVGA, P_HD, P_3MP, QXGA and 5MP give no frames on this board — excluded
const FS=[[0,'96x96'],[1,'QQVGA 160x120'],[3,'QCIF 176x144'],[4,'HQVGA 240x176'],[5,'240x240'],[6,'QVGA 320x240'],[8,'CIF 400x296'],[9,'HVGA 480x320'],[10,'VGA 640x480'],[12,'XGA 1024x768'],[13,'HD 1280x720'],[14,'SXGA 1280x1024'],[15,'UXGA 1600x1200'],[16,'FHD 1920x1080'],[20,'QHD 2560x1440'],[21,'WQXGA 2560x1600'],[22,'P_FHD 1080x1920'],[23,'QSXGA 2560x1920']];
// [id, label, type, min|options, max]
const GROUPS=[
 ['Image',[
  ['framesize','Resolution','sel',FS],
  ['quality','JPEG quality','rng',4,63],
  ['brightness','Brightness','rng',-2,2],
  ['contrast','Contrast','rng',-2,2],
  ['saturation','Saturation','rng',-2,2],
  ['sharpness','Sharpness','rng',-2,2],
  ['special_effect','Effect','sel',[[0,'None'],[1,'Negative'],[2,'Grayscale'],[3,'Red tint'],[4,'Green tint'],[5,'Blue tint'],[6,'Sepia']]],
  ['hmirror','Mirror','chk'],
  ['vflip','Flip','chk'],
 ]],
 ['White balance',[
  ['awb','Auto WB','chk'],
  ['awb_gain','AWB gain','chk'],
  ['wb_mode','WB mode','sel',[[0,'Auto'],[1,'Sunny'],[2,'Cloudy'],[3,'Office'],[4,'Home']]],
 ]],
 ['Exposure & gain',[
  ['aec','Auto exposure','chk'],
  ['aec2','AEC DSP','chk'],
  ['ae_level','AE level','rng',-2,2],
  ['aec_value','Exposure','rng',0,1200],
  ['agc','Auto gain','chk'],
  ['agc_gain','Gain','rng',0,30],
  ['gainceiling','Gain ceiling','sel',[[0,'2x'],[1,'4x'],[2,'8x'],[3,'16x'],[4,'32x'],[5,'64x'],[6,'128x']]],
 ]],
 ['Corrections',[
  ['bpc','Black pixel (BPC)','chk'],
  ['wpc','White pixel (WPC)','chk'],
  ['raw_gma','Gamma','chk'],
  ['lenc','Lens correction','chk'],
  ['dcw','Downsize (DCW)','chk'],
  ['denoise','Denoise','rng',0,8],
  ['colorbar','Color bars','chk'],
 ]],
 ['Board',[
  ['led','LED (IO25)','chk'],
 ]],
];
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('show'),1800)}
function build(){
  const box=$('ctl');
  GROUPS.forEach(([title,items],gi)=>{
    const d=document.createElement('details');if(gi<3)d.open=true;
    d.innerHTML='<summary>'+title+'</summary>';
    items.forEach(([id,label,type,a,b])=>{
      const r=document.createElement('div');r.className='row';r.id='r_'+id;
      let inp;
      if(type==='rng'){inp='<input type="range" id="'+id+'" min="'+a+'" max="'+b+'" step="1">'}
      else if(type==='sel'){inp='<select id="'+id+'">'+a.map(o=>'<option value="'+o[0]+'">'+o[1]+'</option>').join('')+'</select>'}
      else{inp='<label class="sw"><input type="checkbox" id="'+id+'"><span></span></label>'}
      r.innerHTML='<label for="'+id+'">'+label+'</label>'+inp+'<span class="v" id="v_'+id+'"></span>';
      d.appendChild(r);
      const el=r.querySelector('#'+id);
      if(type==='rng')el.addEventListener('input',()=>$('v_'+id).textContent=el.value);
      el.addEventListener('change',()=>send(id,type==='chk'?(el.checked?1:0):el.value));
    });
    box.appendChild(d);
  });
  const d=document.createElement('details');d.innerHTML='<summary>System</summary>';
  const rc=document.createElement('button');rc.textContent='Restart camera';rc.style.margin='6px 8px 6px 0';
  rc.title='Use if the picture is truncated or the stream stutters';
  rc.onclick=async()=>{const w=running;stop();toast('Restarting camera… (up to ~30 s)');
    try{const t=await (await fetch(base+'/control?var=reinit&val=1')).text();toast(t.trim().split('\n').pop())}catch(e){toast('Board not reachable')}
    loadStatus();if(w)start()};
  d.appendChild(rc);
  const bt=document.createElement('button');bt.textContent='Reboot board';bt.style.margin='6px 0';
  bt.onclick=()=>{if(confirm('Reboot the ESP32?')){fetch(base+'/control?var=reboot&val=1');toast('Rebooting…');stop();setTimeout(()=>location.reload(),8000)}};
  d.appendChild(bt);box.appendChild(d);
}
async function send(id,val){
  try{
    const r=await fetch(base+'/control?var='+id+'&val='+val);
    $('r_'+id).classList.toggle('err',!r.ok);
    toast(r.ok?id+' = '+val:id+': not supported by the sensor');
    if(r.ok&&id==='framesize'&&running){stop();setTimeout(start,300)}
    loadStatus();
  }catch(e){toast('Board not reachable')}
}
function apply(s){
  GROUPS.forEach(([,items])=>items.forEach(([id,,type])=>{
    if(!(id in s))return;const el=$(id);if(!el||document.activeElement===el)return;
    if(type==='chk')el.checked=!!s[id];
    else{if(type==='sel'&&![...el.options].some(o=>o.value==s[id])){const o=document.createElement('option');o.value=s[id];o.textContent='('+s[id]+')';el.appendChild(o)}el.value=s[id]}
    if(type==='rng')$('v_'+id).textContent=s[id];
  }));
}
function fmtUp(t){const h=Math.floor(t/3600),m=Math.floor(t%3600/60),s=t%60;return (h?h+'h ':'')+m+'m '+s+'s'}
async function loadStatus(){
  try{
    const s=await (await fetch(base+'/status')).json();
    apply(s);
    $('s_fps').textContent=s.fps.toFixed(1);
    $('s_rssi').textContent=s.rssi+' dBm';
    $('s_ip').textContent=s.ip;
    $('s_heap').textContent=(s.heap/1024).toFixed(0)+' KB';
    $('s_psram').textContent=(s.psram_free/1048576).toFixed(1)+'/'+(s.psram/1048576).toFixed(1)+' MB';
    $('s_up').textContent=fmtUp(s.uptime);
    $('s_sd').textContent=s.sd?(s.sd_used+'/'+s.sd_total+' MB'):'none';
  }catch(e){}
}
let running=false;
const BLANK='data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';
function showMsg(t){$('msg').textContent=t;$('msg').style.display=t?'':'none'}
function start(){$('stream').src=streamUrl+'?t='+Date.now();running=true;showMsg('');$('b_stream').textContent='Stop'}
function stop(){$('stream').src=BLANK;running=false;$('b_stream').textContent='Start'}
$('b_stream').onclick=()=>running?stop():start();
$('b_still').onclick=()=>{stop();showMsg('');$('stream').src=base+'/capture?t='+Date.now()};
$('b_save').onclick=()=>{const a=document.createElement('a');a.href=base+'/capture?t='+Date.now();a.download='esp32cam_'+new Date().toISOString().replace(/[:.]/g,'-')+'.jpg';a.click()};
$('b_full').onclick=()=>{const v=document.querySelector('.view');(v.requestFullscreen||v.webkitRequestFullscreen).call(v)};
$('stream').onerror=()=>{const s=$('stream').src;if(s===BLANK)return;
  if(running){stop();showMsg('Stream unavailable (port 81) — press Start')}else showMsg('Snapshot failed')};
build();loadStatus();setInterval(loadStatus,2000);start();
</script>
</body>
</html>
)rawliteral";
