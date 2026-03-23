#ifndef CONFIG_PAGE_H
#define CONFIG_PAGE_H

#include <pgmspace.h>

const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenScale Setup</title>
<style>
  *{box-sizing:border-box}
  body{font-family:sans-serif;max-width:560px;margin:0 auto;padding:30px 20px;background:#111;color:#eee}
  h1{color:#4CAF50;margin-bottom:5px;font-size:clamp(22px,3.5vw,32px)}
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
  .spinner{width:40px;height:40px;border:4px solid #333;border-top:4px solid #4CAF50;border-radius:50%;animation:spin .8s linear infinite}
  @keyframes spin{to{transform:rotate(360deg)}}
  .content{display:none}
</style>
</head><body>
<h1>OpenScale</h1>
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
      <input name="name" id="pf_name" placeholder="z.B. Rico" required>
      <label>Geschlecht</label>
      <select name="gender" id="pf_gender">
        <option value="0">Maennlich</option>
        <option value="1">Weiblich</option>
      </select>
      <label>Alter (Jahre)</label>
      <input name="age" id="pf_age" type="number" min="10" max="99" placeholder="z.B. 35">
      <label>Groesse (cm)</label>
      <input name="height" id="pf_height" type="number" min="100" max="250" placeholder="z.B. 180">
      <button type="submit">Profil speichern</button>
      <div class="msg" id="profile-msg"></div>
    </form>
  </div>
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
  <h2>REST API</h2>
  <p style="color:#999;font-size:14px;margin:0">Messdaten per API abrufen:</p>
  <a href="/api/last-weight-data" target="_blank" style="display:block;margin-top:10px;color:#4CAF50;font-size:15px;word-break:break-all">/api/last-weight-data</a>
  <p style="color:#666;font-size:12px;margin:6px 0 0">Dokumentation: <a href="/api/docs" target="_blank" style="color:#888">/api/docs</a></p>
</div>

<p class="info" style="text-align:center;margin-top:20px">AP schaltet sich nach 5 Min. automatisch ab.</p>
</div>
<script>
var loader=document.getElementById('loader');
var content=document.getElementById('content');
var pending=0;
var initialized=false;
function busy(){
  pending++;
  loader.style.display='flex';
  loader.style.padding=initialized?'12px 0':'80px 0';
}
function done(){
  pending--;
  if(pending<=0){
    pending=0;
    loader.style.display='none';
    content.style.display='block';
    initialized=true;
  }
}
function api(url,opts){
  busy();
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
    if(d.http_webhook) document.getElementById('http_webhook').value=d.http_webhook;
    if(d.history_url) document.getElementById('history_url').value=d.history_url;
    renderProfiles(d.profiles||[]);
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
  if(!confirm(name+' loeschen?'))return;
  api('/delete/profile',{method:'POST',body:'name='+encodeURIComponent(name),headers:{'Content-Type':'application/x-www-form-urlencoded'}})
    .then(()=>loadSettings()).catch(()=>{});
}
function editProfile(name,g,a,h){
  document.getElementById('pf_name').value=name;
  document.getElementById('pf_gender').value=g;
  document.getElementById('pf_age').value=a;
  document.getElementById('pf_height').value=h;
}
loadSettings();
document.querySelectorAll('form[action="/save/mqtt"],form[action="/save/http"],form[action="/save/history-app"]').forEach(function(f){
  f.onsubmit=function(e){
    e.preventDefault();
    var fd=new FormData(f);
    var id=f.action.includes('mqtt')?'mqtt-msg':f.action.includes('history')?'history-msg':'http-msg';
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
</script>
</body></html>
)rawliteral";

#endif
