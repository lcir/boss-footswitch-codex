const presetButtons=[["A1","preset.select","A1"],["A2","preset.select","A2"],["B1","preset.select","B1"],["B2","preset.select","B2"]];
const effectButtons=[["Booster","effect.toggle","booster"],["Mod","effect.toggle","mod"],["FX","effect.toggle","fx"],["Delay","effect.toggle","delay"]];
const diagnostics=[];
const MAX_DIAGNOSTICS=48;
const state={footswitchMode:"preset",activePreset:"A1",effects:{booster:false,mod:false,fx:false,delay:false,reverb:false},synced:false,wifiProvisioned:false,bleState:"disconnected",wifiState:"unprovisioned",midiConfigured:false,pcOffsetMode:"unknown",ampStateConfidence:"none"};
const midiConfig={pcPanel:5,pcA1:1,pcA2:2,pcB1:6,pcB2:7,ccBooster:16,ccMod:17,ccFx:18,ccDelay:19,ccReverb:20,ccSendReturn:21,pcOffsetMode:"unknown"};
const grid=document.getElementById("footswitchGrid");
const fields=["pcPanel","pcA1","pcA2","pcB1","pcB2","ccBooster","ccMod","ccFx","ccDelay","ccReverb","ccSendReturn"];

async function postJSON(url,body){
  const res=await fetch(url,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
  if(!res.ok) throw new Error(await res.text());
  return res.headers.get("content-type")?.includes("application/json")?res.json():res.text();
}

async function send(action,payload={}){
  return postJSON("/api/action",{action,...payload});
}

function applyOptimisticAction(action,payload={}){
  switch(action){
    case "preset.select":
      if(payload.preset){
        state.activePreset=payload.preset;
        state.footswitchMode="preset";
        state.ampStateConfidence="low";
        state.synced=false;
      }
      break;
    case "panel.select":
      state.footswitchMode="preset";
      state.ampStateConfidence="low";
      state.synced=false;
      break;
    case "mode.set":
      if(payload.mode){
        state.footswitchMode=payload.mode;
        state.synced=false;
      }
      break;
    case "effect.toggle":
      if(payload.effect && Object.prototype.hasOwnProperty.call(state.effects,payload.effect)){
        state.effects[payload.effect]=!state.effects[payload.effect];
        state.ampStateConfidence="low";
        state.synced=false;
      }
      break;
    case "ble.reconnect":
      state.bleState="connecting";
      state.ampStateConfidence="none";
      state.synced=false;
      break;
    case "wifi.reset":
      state.wifiProvisioned=false;
      state.wifiState="ap";
      state.ampStateConfidence="none";
      state.synced=false;
      break;
    case "resync":
      state.ampStateConfidence="none";
      state.synced=false;
      break;
    default:
      break;
  }
}

async function sendAction(action,payload={},options={}){
  const optimistic=options.optimistic!==false;
  if(optimistic){
    applyOptimisticAction(action,payload);
    renderButtons();
    renderStatus();
  }

  try{
    await send(action,payload);
    await loadState();
  }catch(err){
    loadState().catch(()=>{});
    throw err;
  }
}

function formatDiagTime(ms){
  const seconds=ms/1000;
  return seconds<10?seconds.toFixed(1)+"s":Math.round(seconds)+"s";
}

function renderButtons(){
  grid.innerHTML="";
  const entries=state.footswitchMode==="preset"?presetButtons:effectButtons;
  entries.forEach(([label,action,value])=>{
    const btn=document.createElement("button");
    btn.type="button";
    btn.className="switch"+(state.footswitchMode==="effect"?" fx":"");
    const active=state.footswitchMode==="preset"?state.activePreset===value:state.effects[value];
    if(active) btn.classList.add("active");
    if(state.footswitchMode==="preset"&&!state.midiConfigured) btn.disabled=true;
    btn.innerHTML="<strong>"+label+"</strong><span>"+(state.footswitchMode==="preset"?"Program Change":"Control Change")+"</span>";
    btn.onclick=()=>sendAction(action,state.footswitchMode==="preset"?{preset:value}:{effect:value}).catch(showError);
    grid.appendChild(btn);
  });

  const panelBtn=document.getElementById("panelButton");
  panelBtn.disabled=!state.midiConfigured;
  panelBtn.title=state.midiConfigured?"":"Calibrate PC offset first";

  const reverbBtn=document.getElementById("reverbButton");
  reverbBtn.classList.toggle("active",state.effects.reverb);
}

function renderStatus(){
  document.getElementById("modeValue").textContent=state.footswitchMode;
  document.getElementById("linkValue").textContent=state.bleState+" / "+state.wifiState;
  document.getElementById("presetValue").textContent=state.activePreset;
  document.getElementById("confidenceValue").textContent=state.ampStateConfidence+(state.synced?"":" (local)");
  document.getElementById("wifiForm").style.display=state.wifiProvisioned?"none":"grid";
  document.getElementById("midiConfiguredBadge").textContent=state.midiConfigured?"ready":"not calibrated";
  document.getElementById("panelProgramBadge").textContent="PC "+midiConfig.pcPanel;
  document.getElementById("offsetBadge").textContent=state.pcOffsetMode;
  document.getElementById("midiConfigHint").innerHTML=state.midiConfigured?'<span class="ok">Preset switching is enabled.</span>':'<span class="warn-text">Preset switching stays disabled until the PC offset is calibrated.</span>';
}

function renderMidiConfig(){
  fields.forEach((key)=>{document.getElementById(key).value=midiConfig[key];});
  document.getElementById("pcOffsetMode").value=midiConfig.pcOffsetMode;
  renderStatus();
}

function renderDiagnostics(){
  const list=document.getElementById("diagList");
  list.innerHTML="";
  if(!diagnostics.length){
    const empty=document.createElement("div");
    empty.className="diag-empty";
    empty.textContent="No diagnostics yet.";
    list.appendChild(empty);
    return;
  }
  diagnostics.slice().reverse().forEach((entry)=>{
    const row=document.createElement("div");
    row.className="diag-entry";
    const time=document.createElement("span");
    time.className="diag-time";
    time.textContent=formatDiagTime(entry.uptimeMs||0);
    const level=document.createElement("span");
    level.className="diag-level diag-"+(entry.level||"info");
    level.textContent=entry.level||"info";
    const source=document.createElement("span");
    source.className="diag-source";
    source.textContent=entry.source||"system";
    const message=document.createElement("span");
    message.textContent=entry.message||"";
    row.append(time,level,source,message);
    list.appendChild(row);
  });
}

function appendDiagnostic(entry){
  diagnostics.push(entry);
  if(diagnostics.length>MAX_DIAGNOSTICS) diagnostics.splice(0,diagnostics.length-MAX_DIAGNOSTICS);
  renderDiagnostics();
}

function applyState(next){
  Object.assign(state,next);
  if(next.effects) state.effects=next.effects;
  renderButtons();
  renderStatus();
}

function applyMidiConfig(next){
  Object.assign(midiConfig,next);
  renderMidiConfig();
}

async function loadState(){
  const res=await fetch("/api/state");
  applyState(await res.json());
}

async function loadMidiConfig(){
  const res=await fetch("/api/midi-config");
  applyMidiConfig(await res.json());
}

async function loadDiagnostics(){
  const res=await fetch("/api/diagnostics");
  const items=await res.json();
  diagnostics.length=0;
  items.forEach((item)=>diagnostics.push(item));
  renderDiagnostics();
}

function collectMidiConfig(){
  const payload={pcOffsetMode:document.getElementById("pcOffsetMode").value};
  fields.forEach((key)=>payload[key]=Number(document.getElementById(key).value));
  return payload;
}

function showError(err){
  alert(typeof err==="string"?err:(err?.message||"Request failed"));
}

async function runCalibration(candidate){
  try{
    await postJSON("/api/midi-calibrate",{candidate,preset:"A1"});
    const ok=confirm("Switched the amp to A1? Click OK to store this offset, Cancel to keep testing.");
    if(ok){
      await postJSON("/api/midi-calibrate",{candidate,preset:"A1",confirm:true});
      await Promise.all([loadMidiConfig(),loadState()]);
    }
  }catch(err){
    showError(err);
  }
}

document.getElementById("panelButton").onclick=()=>sendAction("panel.select").catch(showError);
document.getElementById("modeButton").onclick=()=>sendAction("mode.set",{mode:state.footswitchMode==="preset"?"effect":"preset"}).catch(showError);
document.getElementById("reverbButton").onclick=()=>sendAction("effect.toggle",{effect:"reverb"}).catch(showError);
document.getElementById("reconnectButton").onclick=()=>sendAction("ble.reconnect").catch(showError);
document.getElementById("resyncButton").onclick=()=>sendAction("resync",{},{
  optimistic:false,
}).catch(showError);
document.getElementById("wifiResetButton").onclick=()=>sendAction("wifi.reset").catch(showError);
document.getElementById("midiConfigForm").addEventListener("submit",async(e)=>{
  e.preventDefault();
  try{
    const result=await postJSON("/api/midi-config",collectMidiConfig());
    applyMidiConfig(result);
    await loadState();
  }catch(err){
    showError(err);
  }
});
document.getElementById("testSubtractOne").onclick=()=>runCalibration("subtract-one");
document.getElementById("testDirect").onclick=()=>runCalibration("direct");
document.getElementById("wifiForm").addEventListener("submit",async(e)=>{
  e.preventDefault();
  try{
    await postJSON("/api/provision",{ssid:document.getElementById("ssid").value,password:document.getElementById("password").value});
  }catch(err){
    showError(err);
  }
});

let ws;

function connectWebSocket(){
  ws=new WebSocket((location.protocol==="https:"?"wss://":"ws://")+location.host+"/ws");
  ws.onmessage=(event)=>{
    const data=JSON.parse(event.data);
    if(data.type==="diag"&&data.entry){
      appendDiagnostic(data.entry);
      return;
    }
    applyState(data);
  };
  ws.onclose=()=>setTimeout(()=>{
    loadState().catch(()=>{});
    loadDiagnostics().catch(()=>{});
    connectWebSocket();
  },1500);
}

connectWebSocket();

Promise.all([loadState(),loadMidiConfig(),loadDiagnostics()]).catch(showError);
