#ifndef CONFIG_PAGE_H
#define CONFIG_PAGE_H

#include <pgmspace.h>

const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenScale Setup</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 75.22 75.22' fill='%234CAF50'><path d='M67.104,0H8.117C3.635,0,0,3.634,0,8.117v58.987c0,4.481,3.635,8.116,8.117,8.116h58.987c4.482,0,8.116-3.635,8.116-8.116V8.117C75.22,3.633,71.587,0,67.104,0z M53.922,8.454l-6.035,8.955c-3.585-2.416-7.059-3.207-10.085-3.199l-4.335-7.348l-1.068,0.579l3.419,6.89c-4.787,0.561-8.05,2.904-8.105,2.945l-6.412-8.688C21.939,8.115,37.156-2.845,53.922,8.454z M66.089,58.496c0,4.482-3.634,8.117-8.117,8.117H17.114c-4.483,0-8.117-3.635-8.117-8.117V29.831c0-4.483,3.634-8.117,8.117-8.117h40.857c4.483,0,8.117,3.634,8.117,8.117L66.089,58.496z'/></svg>">
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:30px 20px;background:#111;color:#eee}
  h1{color:#4CAF50;margin-bottom:5px;font-size:20px}
  .home{color:#888;font-size:clamp(13px,1.5vw,15px);text-decoration:none;display:block;margin-bottom:20px}
  .card{background:#1a1a1a;border-radius:10px;padding:clamp(16px,3vw,28px);margin-bottom:18px}
  .card h2{color:#ccc;font-size:clamp(14px,2vw,17px);margin:0 0 14px 0;padding-bottom:8px;border-bottom:1px solid #333}
  input,select{width:100%;padding:clamp(10px,1.5vw,14px);margin:6px 0;border-radius:6px;border:1px solid #444;background:#222;color:#eee;font-size:clamp(14px,1.5vw,16px)}
  label{display:block;margin-top:10px;color:#999;font-size:clamp(12px,1.5vw,14px)}
  button{width:100%;padding:clamp(11px,1.5vw,15px);margin-top:14px;background:#4CAF50;color:#fff;border:none;border-radius:6px;font-size:clamp(15px,1.5vw,18px);cursor:pointer}
  button:hover{background:#45a049}
  .info{color:#888;font-size:clamp(11px,1.2vw,14px);margin-top:6px}
  .msg{padding:10px;border-radius:6px;margin-top:10px;font-size:clamp(13px,1.5vw,15px);display:none}
  .msg.ok{display:block;background:#1b3a1b;color:#4CAF50}
  .msg.err{display:block;background:#3a1b1b;color:#f44336}
  .spinner-wrap{display:flex;justify-content:center;align-items:center;padding:80px 0}
  .spinner-wrap.overlay{position:fixed;top:0;left:0;right:0;bottom:0;padding:0;background:rgba(0,0,0,0.5);z-index:100}
  .spinner{width:40px;height:40px;border:4px solid #333;border-top:4px solid #4CAF50;border-radius:50%;animation:spin .8s linear infinite}
  @keyframes spin{to{transform:rotate(360deg)}}
  .content{display:none}
</style>
</head><body>
<h1><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 75.22 75.22" fill="#4CAF50" style="width:20px;height:20px;vertical-align:-2px;margin-right:6px;transform:rotate(340deg)"><path d="M67.104,0H8.117C3.635,0,0,3.634,0,8.117v58.987c0,4.481,3.635,8.116,8.117,8.116h58.987c4.482,0,8.116-3.635,8.116-8.116V8.117C75.22,3.633,71.587,0,67.104,0z M53.922,8.454l-6.035,8.955c-3.585-2.416-7.059-3.207-10.085-3.199l-4.335-7.348l-1.068,0.579l3.419,6.89c-4.787,0.561-8.05,2.904-8.105,2.945l-6.412-8.688C21.939,8.115,37.156-2.845,53.922,8.454z M66.089,58.496c0,4.482-3.634,8.117-8.117,8.117H17.114c-4.483,0-8.117-3.635-8.117-8.117V29.831c0-4.483,3.634-8.117,8.117-8.117h40.857c4.483,0,8.117,3.634,8.117,8.117L66.089,58.496z"/></svg>OpenScale</h1>
<a class="home" href="/">zur Startseite</a>
<div class="spinner-wrap" id="loader"><div class="spinner"></div></div>
<div class="content" id="content">

<div class="card">
  <h2>Profile</h2>
  <div id="profile-list"></div>
  <div style="margin-top:14px;padding-top:14px;border-top:1px solid #333">
    <div style="color:#999;font-size:13px;margin-bottom:8px">Neues Profil / Bearbeiten</div>
    <form id="profile-form" action="/save/profile" method="POST">
      <label>Name</label>
      <input name="name" id="pf_name" placeholder="z.B. Peter" required>
      <label>Geschlecht</label>
      <select name="gender" id="pf_gender">
        <option value="0">Männlich</option>
        <option value="1">Weiblich</option>
      </select>
      <label>Alter (Jahre)</label>
      <input name="age" id="pf_age" type="number" min="10" max="99" placeholder="z.B. 35">
      <label>Größe (cm)</label>
      <input name="height" id="pf_height" type="number" min="100" max="250" placeholder="z.B. 180">
      <button type="submit">Profil speichern</button>
      <div class="msg" id="profile-msg"></div>
    </form>
  </div>
</div>

<div class="card">
  <h2>Auto-Profil</h2>
  <div class="info" style="margin-bottom:12px">Profil automatisch anhand des Gewichts wählen.<br>- Erste passende Regel greift.</div>
  <form id="auto-profile-form" action="/save/auto-profile" method="POST">
    <label style="display:flex;align-items:center;gap:8px;margin-bottom:10px"><input name="enabled" id="ap_enabled" type="checkbox" value="1" style="width:auto"> Aktiviert</label>
    <div class="info" id="ap-default-hint">Wenn keine Regel greift, wird das aktive Profil als Default genutzt.</div>
    <div id="ap-rules" style="margin-top:14px"></div>
    <button type="button" id="ap-add-rule" style="background:#333;margin-top:8px;font-size:14px">+ Regel hinzufügen</button>
    <button type="submit">Auto-Profil speichern</button>
    <div class="msg" id="auto-profile-msg"></div>
  </form>
</div>

<div class="card">
  <h2>WLAN</h2>
  <form action="/save/wifi" method="POST">
    <label>SSID</label>
    <select name="ssid" id="ssid" style="display:none"></select>
    <input name="ssid_manual" id="ssid_manual" placeholder="WLAN Name">
    <div class="info" id="scan-status">Scanne WLANs...</div>
    <label>Passwort</label>
    <input name="pass" type="password" placeholder="WLAN Passwort">
    <button type="submit">WLAN speichern & verbinden</button>
  </form>
</div>

<div class="card">
  <h2>MQTT</h2>
  <form action="/save/mqtt" method="POST">
    <label>Broker</label>
    <input name="broker" id="mqtt_broker" placeholder="z.B. 192.168.178.50">
    <label>Port</label>
    <input name="port" id="mqtt_port" placeholder="1883" type="number">
    <label>Topic</label>
    <input name="topic" id="mqtt_topic" placeholder="openscale/weight">
    <label>Benutzer (optional)</label>
    <input name="user" id="mqtt_user" placeholder="">
    <label>Passwort (optional)</label>
    <input name="pass" id="mqtt_pass" type="password" placeholder="">
    <label style="display:flex;align-items:center;gap:8px;margin:10px 0"><input name="retain" id="mqtt_retain" type="checkbox" value="1" style="width:auto"> Retained Message</label>
    <button type="submit">MQTT speichern</button>
    <div class="msg" id="mqtt-msg"></div>
  </form>
</div>

<div class="card">
  <h2>HTTP Webhook</h2>
  <form action="/save/http" method="POST">
    <label>POST URL</label>
    <input name="webhook" id="http_webhook" placeholder="https://example.com/webhook">
    <button type="submit">Webhook speichern</button>
    <div class="msg" id="http-msg"></div>
  </form>
</div>

<div class="card">
  <h2>History-App</h2>
  <form action="/save/history-app" method="POST">
    <label>URL zur History-App</label>
    <input name="history_url" id="history_url" placeholder="https://example.com/history">
    <button type="submit">Speichern</button>
    <div class="msg" id="history-msg"></div>
  </form>
</div>

<div class="card">
  <h2>Buzzer</h2>
  <form action="/save/buzzer" method="POST">
    <label>GPIO Pin (0 = deaktiviert)</label>
    <input name="buzzer_pin" id="buzzer_pin" type="number" min="0" max="39" placeholder="z.B. 25">
    <div class="info">Piezo-Buzzer piept 1 Sek. bei erfolgreicher Messung</div>
    <button type="submit">Buzzer speichern</button>
    <div class="msg" id="buzzer-msg"></div>
  </form>
</div>

<div class="card">
  <h2>REST API</h2>
  <p style="color:#999;font-size:14px;margin:0">Messdaten per API abrufen:</p>
  <a href="/api/last-weight-data" target="_blank" style="display:block;margin-top:10px;color:#4CAF50;font-size:15px;word-break:break-all">/api/last-weight-data</a>
  <p style="color:#666;font-size:12px;margin:6px 0 0">Dokumentation: <a href="/api/docs" target="_blank" style="color:#888">/api/docs</a></p>
</div>

<div class="card">
  <h2>Firmware Update (OTA)</h2>
  <form id="ota-form">
    <label>Firmware-Datei (.bin)</label>
    <input type="file" name="firmware" id="ota-file" accept=".bin" required style="padding:10px;background:#222;border:1px solid #444;border-radius:6px;color:#eee">
    <div class="info">Aktuelle Firmware hochladen (firmware.bin aus .pio/build/esp32/)</div>
    <button type="submit">Firmware aktualisieren</button>
    <div id="ota-progress" style="display:none;margin-top:10px">
      <div style="background:#333;border-radius:6px;overflow:hidden;height:24px">
        <div id="ota-bar" style="background:#4CAF50;height:100%;width:0%;transition:width 0.3s;text-align:center;line-height:24px;font-size:13px;color:#fff">0%</div>
      </div>
    </div>
    <div class="msg" id="ota-msg"></div>
  </form>
</div>

<p class="info" style="text-align:center;margin-top:20px">AP schaltet sich nach 5 Min. automatisch ab.</p>
<p class="info" style="text-align:center;margin-top:10px;color:#555" id="build-info"></p>
</div>
<script>
var loader=document.getElementById('loader');
var content=document.getElementById('content');
var pending=0;
var initialized=false;
function busy(){
  pending++;
  loader.style.display='flex';
  if(initialized) loader.classList.add('overlay');
}
function done(){
  pending--;
  if(pending<=0){
    pending=0;
    loader.style.display='none';
    loader.classList.remove('overlay');
    content.style.display='block';
    initialized=true;
  }
}
function api(url,opts){
  busy();
  var c=new AbortController();setTimeout(function(){c.abort()},10000);
  opts=opts||{};opts.signal=c.signal;
  return fetch(url,opts).then(r=>r.json()).finally(done);
}
// Initial load: scan + settings in parallel
api('/api/scan').then(d=>{
  var sel=document.getElementById('ssid');
  var inp=document.getElementById('ssid_manual');
  var st=document.getElementById('scan-status');
  if(d.length>0){
    sel.style.display='';inp.style.display='none';inp.name='';sel.name='ssid';
    d.forEach(function(n){
      var o=document.createElement('option');
      o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)';
      sel.appendChild(o);
    });
    var o=document.createElement('option');
    o.value='__manual__';o.textContent='Manuell eingeben...';
    sel.appendChild(o);
    sel.onchange=function(){
      if(sel.value==='__manual__'){
        inp.style.display='';inp.name='ssid';inp.required=true;sel.name='';
      }else{
        inp.style.display='none';inp.name='';inp.required=false;sel.name='ssid';
      }
    };
    st.textContent=d.length+' Netzwerke gefunden';
  }else{
    st.textContent='Keine Netzwerke gefunden';inp.required=true;
  }
}).catch(()=>{
  document.getElementById('scan-status').textContent='Scan fehlgeschlagen';
  document.getElementById('ssid_manual').required=true;
});
function loadSettings(){
  api('/api/settings').then(d=>{
    if(d.mqtt_broker) document.getElementById('mqtt_broker').value=d.mqtt_broker;
    if(d.mqtt_port) document.getElementById('mqtt_port').value=d.mqtt_port;
    if(d.mqtt_topic) document.getElementById('mqtt_topic').value=d.mqtt_topic;
    if(d.mqtt_user) document.getElementById('mqtt_user').value=d.mqtt_user;
    if(d.mqtt_retain) document.getElementById('mqtt_retain').checked=true;
    if(d.http_webhook) document.getElementById('http_webhook').value=d.http_webhook;
    if(d.history_url) document.getElementById('history_url').value=d.history_url;
    if(d.buzzer_pin) document.getElementById('buzzer_pin').value=d.buzzer_pin;
    if(d.build_time) document.getElementById('build-info').textContent='Build: '+d.build_time;
    renderProfiles(d.profiles||[]);
    // Auto-profile
    var ap=d.profiles?d.profiles.find(function(p){return p.active;}):'';
    document.getElementById('ap-default-hint').textContent='Wenn keine Regel greift, wird das aktive Profil'+(ap?' ('+ap.name+')':'')+' als Default genutzt.';
    document.getElementById('ap_enabled').checked=!!d.auto_profile_enabled;
    toggleApFields();
    var rules=d.auto_profile_rules||[];
    document.getElementById('ap-rules').innerHTML='';
    apIdx=0;
    rules.forEach(function(r){addApRule(r.min,r.max,r.profile);});
  }).catch(()=>{});
}
function renderProfiles(profiles){
  var el=document.getElementById('profile-list');
  if(!profiles.length){el.innerHTML='<div style="color:#666;font-size:13px">Keine Profile vorhanden</div>';return;}
  var h='';
  profiles.forEach(function(p){
    var active=p.active;
    h+='<div style="display:flex;align-items:center;gap:10px;padding:10px;margin-bottom:6px;background:'+(active?'#1b3a1b':'#222')+';border-radius:8px;border:1px solid '+(active?'#4CAF50':'#333')+'">';
    h+='<div style="flex:1"><strong style="color:'+(active?'#4CAF50':'#eee')+'">'+p.name+'</strong>';
    h+='<div style="color:#888;font-size:12px">'+(p.gender==0?'M':'W')+', '+p.age+' J., '+p.height+' cm</div></div>';
    if(!active) h+='<button onclick="setActive(\''+p.name+'\')" style="width:auto;padding:6px 12px;font-size:13px;margin:0;background:#333;color:#eee">Aktivieren</button>';
    h+='<button onclick="editProfile(\''+p.name+'\','+p.gender+','+p.age+','+p.height+')" style="width:auto;padding:6px 12px;font-size:13px;margin:0;background:#333;color:#eee">Bearbeiten</button>';
    h+='<button onclick="delProfile(\''+p.name+'\')" style="width:auto;padding:6px 10px;font-size:13px;margin:0;background:#3a1b1b;color:#f44">X</button>';
    h+='</div>';
  });
  el.innerHTML=h;
}
function setActive(name){
  api('/api/set-profile',{method:'POST',body:'name='+encodeURIComponent(name),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(()=>loadSettings()).catch(()=>{});
}
function delProfile(name){
  if(!confirm(name+' löschen?'))return;
  api('/delete/profile',{method:'POST',body:'name='+encodeURIComponent(name),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(()=>loadSettings()).catch(()=>{});
}
function editProfile(name,g,a,h){
  document.getElementById('pf_name').value=name;
  document.getElementById('pf_gender').value=g;
  document.getElementById('pf_age').value=a;
  document.getElementById('pf_height').value=h;
}
var apIdx=0;
function addApRule(mn,mx,pf){
  if(apIdx>=8)return;
  var i=apIdx++;
  var d=document.createElement('div');
  d.style.cssText='display:flex;gap:6px;align-items:center;margin-bottom:6px';
  d.innerHTML='<input name="r'+i+'_min" type="number" step="0.1" min="0" placeholder="Min kg" value="'+(mn||'')+'" style="flex:1">'
    +'<input name="r'+i+'_max" type="number" step="0.1" min="0" placeholder="Max kg" value="'+(mx||'')+'" style="flex:1">'
    +'<input name="r'+i+'_prof" placeholder="Profil" value="'+(pf||'')+'" style="flex:1.2">'
    +'<button type="button" onclick="this.parentElement.remove()" style="width:auto;padding:6px 10px;margin:0;background:#3a1b1b;color:#f44;font-size:13px;flex-shrink:0">X</button>';
  document.getElementById('ap-rules').appendChild(d);
}
document.getElementById('ap-add-rule').onclick=function(){if(apIdx<8)addApRule();else alert('Max. 8 Regeln');};
function toggleApFields(){
  var on=document.getElementById('ap_enabled').checked;
  document.getElementById('ap-rules').style.opacity=on?'1':'0.4';
  document.getElementById('ap-rules').style.pointerEvents=on?'':'none';
  document.getElementById('ap-add-rule').disabled=!on;
  document.getElementById('ap-add-rule').style.opacity=on?'1':'0.4';
}
document.getElementById('ap_enabled').onchange=toggleApFields;
loadSettings();
document.getElementById('auto-profile-form').onsubmit=function(e){
  e.preventDefault();
  var fd=new FormData(this);
  api('/save/auto-profile',{method:'POST',body:new URLSearchParams(fd)})
    .then(d=>{
      var m=document.getElementById('auto-profile-msg');
      m.textContent=d.message;m.className='msg '+(d.ok?'ok':'err');
    }).catch(()=>{});
};
document.querySelectorAll('form[action="/save/mqtt"],form[action="/save/http"],form[action="/save/buzzer"],form[action="/save/history-app"]').forEach(function(f){
  f.onsubmit=function(e){
    e.preventDefault();
    var fd=new FormData(f);
    var id=f.action.includes('mqtt')?'mqtt-msg':f.action.includes('buzzer')?'buzzer-msg':f.action.includes('history')?'history-msg':'http-msg';
    api(f.action,{method:'POST',body:new URLSearchParams(fd)})
      .then(d=>{
        var m=document.getElementById(id);
        m.textContent=d.message;
        m.className='msg '+(d.ok?'ok':'err');
      }).catch(()=>{});
  };
});
document.getElementById('profile-form').onsubmit=function(e){
  e.preventDefault();
  var fd=new FormData(this);
  api('/save/profile',{method:'POST',body:new URLSearchParams(fd)})
    .then(d=>{
      var m=document.getElementById('profile-msg');
      m.textContent=d.message;m.className='msg '+(d.ok?'ok':'err');
      if(d.ok){document.getElementById('profile-form').reset();loadSettings();}
    }).catch(()=>{});
};
document.getElementById('ota-form').onsubmit=function(e){
  e.preventDefault();
  var file=document.getElementById('ota-file').files[0];
  if(!file)return;
  if(!confirm('Firmware wirklich aktualisieren? Das Gerät startet danach neu.'))return;
  var fd=new FormData();
  fd.append('firmware',file);
  var xhr=new XMLHttpRequest();
  var bar=document.getElementById('ota-bar');
  var prog=document.getElementById('ota-progress');
  var msg=document.getElementById('ota-msg');
  prog.style.display='block';
  msg.className='msg';msg.style.display='none';
  xhr.upload.onprogress=function(ev){
    if(ev.lengthComputable){
      var pct=Math.round(ev.loaded/ev.total*100);
      bar.style.width=pct+'%';bar.textContent=pct+'%';
    }
  };
  xhr.onload=function(){
    try{
      var d=JSON.parse(xhr.responseText);
      msg.textContent=d.message;msg.className='msg '+(d.ok?'ok':'err');
      if(d.ok) setTimeout(function(){location.reload();},3000);
    }catch(ex){msg.textContent='Unerwartete Antwort';msg.className='msg err';}
  };
  xhr.onerror=function(){msg.textContent='Upload fehlgeschlagen';msg.className='msg err';};
  xhr.open('POST','/ota');
  xhr.send(fd);
};
</script>
</body></html>
)rawliteral";

#endif
