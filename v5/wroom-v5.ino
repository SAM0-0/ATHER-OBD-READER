#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>

#define CAN_TX 23
#define CAN_RX 22
const char* AP_SSID = "A-OBD";
const char* AP_PASS = "12345678";

// ---- Offline log (WROOM only): 1 Hz ring buffer, capped at 1 hour -------------
#define LOG_CAP          3600    // max UNACKNOWLEDGED samples on the ESP32 (3600 @ 1 Hz = 1 hour, ~43 KB)
#define LOG_INTERVAL_MS  1000    // sampling period
#define LOG_MAX_PER_REQ  300     // max samples per /log response

struct Telemetry {
  uint16_t soc;
  uint16_t soh;
  uint16_t deltaSoc;
  uint16_t voltage;
  uint16_t imbalance;
  int      balancing;
  int      rpm;
  int      driveMode;
  bool     sideStand;
  bool     frontBrake, rearBrake, highBeam, startSwitch;
  bool     killSwitch, storageSwitch, horn;
  bool     indicatorLeftSide, indicatorRightSide, indicatorCenterSide, keyOn;
  int      rangeKm;
  uint16_t cellV[14];
  uint8_t  cellSoh[14];
  uint32_t chargerOV;
  uint32_t chargerOI;
  uint32_t chargerIV;
  int16_t  motorI;
  int16_t  motorTemp;
  bool     displayFan;
  uint16_t chargerFan1, chargerFan2;
  bool     forcedEco, smartEco;
  bool     seatLock, chargerLock1, chargerLock2;
  bool     chargerConnected;
  int16_t  controllerTemp;
  int16_t  throttleOut, throttle1, throttle3;
  uint16_t humidity;
  int16_t  ambientTemp;
  uint16_t maxCellSoh, minCellSoh;
  uint16_t pduVoltage;
  int16_t  cellTemp[5];
  int16_t  afeDieTemp, packCurrent;
  int16_t  eftTemp, eciTemp, senseTemp;
  uint16_t deratedDsgI, deratedChgI, deratedRegenI, dsgLimTemp;
  uint16_t chgCellRule, chgCmdRateLtd;
  int16_t  chgPostQuant;
  uint16_t soe, socEkf, maxSoc, minSocEst;
  int16_t  minV1Est, minV2Est;
  uint16_t cellSoc[14];
  uint32_t odometer;
  uint8_t  bmsHwVer, bmsFw[3], bcmFw[3], gtwFw[3], motorFw[3];
  char     binId[16];
};

Telemetry snap;
SemaphoreHandle_t mtx;
WebServer server(80);
volatile bool chartOn = false;
volatile uint16_t chartId = 0;
volatile unsigned long chartUntil = 0;

// ---- Offline log state (only touched from the Arduino loop task) -------------
struct LogSample {
  uint32_t t;          // device uptime, seconds
  uint16_t voltage;    // pack voltage, 0.01 V units (same as snap.voltage)
  uint16_t soc;        // state of charge, 0.01 % units (same as snap.soc)
  int16_t  motorTemp;  // deg C
  uint8_t  range;      // km
  uint8_t  mode;       // raw drive mode code
  int16_t  motorI;     // motor battery current, 0.1 A units (signed)
  uint16_t rpm;        // motor RPM
  uint32_t odo;        // odometer, metres (same as snap.odometer)
};
static LogSample logBuf[LOG_CAP];
static uint32_t logHead  = 0;   // sequence number of newest sample (0 = none yet)
static uint32_t logSent  = 0;   // highest sequence number transmitted to a browser
static uint32_t logAcked = 0;   // highest sequence number the browser confirmed (everything <= this is deleted)
static uint32_t logBootId = 0;  // random per boot so the browser can detect restarts
unsigned long lastLogSample = 0;

inline const char* driveModeName(int v){
  switch(v){
    case 1: return "SPORTS";
    case 2: return "DRIVE";
    case 3: return "ECO";
    case 6: return "WARP";
    case 8: return "S. ECO";
    default: return "UNKNOWN";
  }
}

const char htmlPage[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>A OBD Scanner v5</title>
<style>
:root{--bg:#eceeef;--card:#fff;--text:#2b2e34;--muted:#6b7280;--border:#e2e6ea;--accent:#5a7a6a;}
*{box-sizing:border-box}
body{font-family:Inter,Arial,sans-serif;background:var(--bg);color:var(--text);margin:0;}
header{background:var(--card);border-bottom:1px solid var(--border);padding:14px 10px;text-align:center;}
header h1{margin:0;font-size:22px;}
.tabs{display:flex;justify-content:center;background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:10;}
.tab{flex:1;max-width:160px;padding:12px 6px;border:none;background:var(--card);color:var(--muted);font-size:13px;font-weight:600;cursor:pointer;border-bottom:3px solid transparent;transition:all .2s;}
.tab.active{color:var(--accent);border-bottom-color:var(--accent);background:#f8f9fa;}
.tab:hover{background:#f3f4f5;}
.card{max-width:520px;margin:16px auto;background:var(--card);border-radius:14px;overflow:hidden;border:1px solid var(--border);box-shadow:0 2px 10px rgba(0,0,0,0.06);}
.card h2{margin:0;padding:10px;background:#f3f4f5;color:var(--muted);font-size:13px;letter-spacing:0.6px;text-transform:uppercase;border-bottom:1px solid var(--border);}
table{width:100%;border-collapse:collapse;font-size:15px;}
td{padding:10px 16px;border-bottom:1px solid #f0f1f2;text-align:left;}
td:first-child{color:var(--muted);}
td.v{color:var(--accent);font-weight:700;text-align:right;}
td.on{color:var(--accent);font-weight:700;text-align:right;}
td.off{color:#9aa0a8;text-align:right;}
.ctable{max-width:700px;margin:16px auto;}
.ctable table{font-size:14px;}
.ctable td{padding:8px 12px;text-align:center;}
.ctable th{padding:8px 12px;text-align:center;font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:0.5px;border-bottom:1px solid var(--border);}
.ctable tr:nth-child(even){background:#f8f9fa;}
.ctable tr:hover{background:#eef1f3;}
.ctable td:first-child{color:var(--muted);font-weight:600;}
.ctable td.cv{color:var(--accent);font-weight:700;}
.panel{display:none;}
.panel.active{display:block;}
.subtab{display:flex;gap:8px;justify-content:center;padding:14px 10px 0;}
.subtab button{flex:1;max-width:220px;padding:10px;border:1px solid var(--border);background:var(--card);color:var(--muted);font-size:13px;font-weight:600;border-radius:10px;cursor:pointer;}
.subtab button.active{color:var(--accent);border-color:var(--accent);background:#eef2ef;}
.offbtn{max-width:none;flex:none;padding:10px 18px;}
</style>
<script>
let curTab='dash',busy=false;
function showTab(t){
  if(curTab==='chart'&&t!=='chart') chartStop(true);
  curTab=t;
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(b=>b.classList.remove('active'));
  document.getElementById(t).classList.add('active');
  document.getElementById('tb_'+t).classList.add('active');
  if(t!=='chart') updateAll();
}
let curSub='live';
function showSub(s){
  curSub=s;
  document.getElementById('sub_live').classList.toggle('active',s==='live');
  document.getElementById('sub_off').classList.toggle('active',s==='off');
  document.getElementById('subLive').style.display=(s==='live')?'':'none';
  document.getElementById('subOff').style.display=(s==='off')?'':'none';
  if(s==='off') offOpen();
}
function sw(id,val,onTxt,offTxt){let e=document.getElementById(id);if(!e)return;e.innerText=val?onTxt:offTxt;e.className=val?'on':'off';}
async function updateAll(){
 if(curTab==='chart'||busy)return;
 busy=true;
 try{
  let r=await fetch('/all');let j=await r.json();
  const d=j.d,c=j.c,ch=j.ch,x=j.x;
  if(curTab==='dash'){
   document.getElementById('soc').innerText=d.soc.toFixed(2)+' %';
   document.getElementById('soh').innerText=d.soh.toFixed(2)+' %';
   document.getElementById('bdelta').innerText=d.bdelta.toFixed(2)+' %';
   document.getElementById('volt').innerText=d.volt.toFixed(2)+' V';
   document.getElementById('imb').innerText=d.imb.toFixed(4)+' V';
   document.getElementById('mode').innerText=d.mode;
   document.getElementById('range').innerText=d.range.toFixed(0)+' km';
   sw('fbrake',d.fbrake,'Pressed','Not Pressed');
   sw('rbrake',d.rbrake,'Pressed','Not Pressed');
   sw('hbeam',d.hbeam,'Pressed','Not Pressed');
   sw('start',d.start,'Pressed','Not Pressed');
   sw('kill',d.kill,'Pressed','Not Pressed');
   sw('storage',d.storage,'Pressed','Not Pressed');
   sw('horn',d.horn,'Pressed','Not Pressed');
   sw('indl',d.indl,'Pressed','Not Pressed');
   sw('indr',d.indr,'Pressed','Not Pressed');
   sw('indc',d.indc,'Pressed','Not Pressed');
   sw('key',d.key,'Enabled','Disabled');
   sw('stand',d.stand,'DOWN','UP');
   sw('seatlock',d.seat,'Locked','Unlocked');
  }else if(curTab==='cells'){
   for(let i=0;i<14;i++){
     document.getElementById('sh'+i).innerText=c.h[i].toFixed(0)+' %';
     document.getElementById('sc'+i).innerText=(c.s[i]/100).toFixed(2)+' %';
     document.getElementById('cv'+i).innerText=c.v[i].toFixed(4)+' V';
   }
   document.getElementById('cmax').innerText=c.maxS.toFixed(2)+' %';
   document.getElementById('cmin').innerText=c.minS.toFixed(2)+' %';
  }else if(curTab==='charger'){
   document.getElementById('coutv').innerText=ch.coutv.toFixed(2)+' V';
   document.getElementById('couti').innerText=ch.couti.toFixed(2)+' A';
   document.getElementById('cinv').innerText=ch.cinv.toFixed(2)+' V';
   document.getElementById('bal').innerText=ch.bal;
   document.getElementById('cfan1').innerText=ch.fan1;
   document.getElementById('cfan2').innerText=ch.fan2;
   sw('cconnected',ch.cconnected,'Connected','Disconnected');
   sw('clock1',ch.clock1,'Locked','Unlocked');
   sw('clock2',ch.clock2,'Locked','Unlocked');
   let w=ch.watts;
   document.getElementById('cwatts').innerText=w>=1000?(w/1000).toFixed(2)+' kW':w.toFixed(1)+' W';
  }else if(curTab==='others'){
   sw('feco',d.feco,'ON','OFF');
   sw('seco',d.seco,'ON','OFF');
   document.getElementById('rpm').innerText=d.rpm;
   sw('dfan',d.dfan,'ON','OFF');
   document.getElementById('hum').innerText=d.hum.toFixed(2)+' %';
   document.getElementById('amb').innerText=d.amb.toFixed(2)+' \u00B0C';
   document.getElementById('mbi').innerText=ch.mbi.toFixed(2)+' A';
   document.getElementById('mt').innerText=ch.mt.toFixed(1)+' \u00B0C';
   document.getElementById('ctemp').innerText=ch.ctemp.toFixed(0)+' \u00B0C';
   document.getElementById('thro').innerText=ch.thro.toFixed(0)+' %';
   document.getElementById('thr1').innerText=ch.thr1.toFixed(0)+' %';
   document.getElementById('thr3').innerText=ch.thr3.toFixed(0)+' %';
   document.getElementById('pdu').innerText=ch.pdu.toFixed(2)+' V';
  }else if(curTab==='extra'){
   const T=v=>(v/100).toFixed(2)+' \u00B0C';
   for(let i=0;i<5;i++) document.getElementById('xct'+i).innerText=T(x.ct[i]);
   document.getElementById('xdie').innerText=T(x.die);
   document.getElementById('xeft').innerText=T(x.eft);
   document.getElementById('xeci').innerText=T(x.eci);
   document.getElementById('xsr').innerText=T(x.sr);
   document.getElementById('xci').innerText=(x.ci/100).toFixed(2)+' A';
   document.getElementById('xsoe').innerText=(x.soe/100).toFixed(2)+' %';
   document.getElementById('xsekf').innerText=(x.sekf/100).toFixed(2)+' %';
   document.getElementById('xmsoc').innerText=(x.msoc/100).toFixed(2)+' %';
   document.getElementById('xminsoc').innerText=(x.minsoc/100).toFixed(2)+' %';
   document.getElementById('xv1').innerText=(x.v1/10000).toFixed(4)+' V';
   document.getElementById('xv2').innerText=(x.v2/10000).toFixed(4)+' V';
   document.getElementById('xdd').innerText=(x.dd/100).toFixed(2)+' A';
   document.getElementById('xdc').innerText=(x.dc/100).toFixed(2)+' A';
   document.getElementById('xdr').innerText=(x.dr/100).toFixed(2)+' A';
   document.getElementById('xdl').innerText=(x.dl/100).toFixed(2)+' A';
   document.getElementById('xcr').innerText=(x.cr/100).toFixed(2)+' A';
   document.getElementById('xcrl').innerText=(x.crl/100).toFixed(2)+' A';
   document.getElementById('xcpq').innerText=(x.cpq/100).toFixed(2)+' A';
   document.getElementById('xodo').innerText=(x.odo/1000).toFixed(1)+' km';
   document.getElementById('xbhw').innerText=x.bhw;
   const V=a=>a[0]+'.'+a[1]+'.'+a[2];
   document.getElementById('xbfw').innerText=V(x.bfw);
   document.getElementById('xcfw').innerText=V(x.cfw);
   document.getElementById('xgfw').innerText=V(x.gfw);
   document.getElementById('xmfw').innerText=V(x.mfw);
   document.getElementById('xbin').innerText=x.bin||'--';
  }
 }catch(e){}
 busy=false;
}
let chartTimer=null,chartSamples=[],chartT0=0,chartField='rpm';
const chartMaxPts=90;
function chartMeta(){let s=document.getElementById('chartField');return{text:s.options[s.selectedIndex].text,unit:s.options[s.selectedIndex].dataset.unit};}
function chartStart(){
 if(chartTimer)chartStop(true);
 chartField=document.getElementById('chartField').value;
 document.getElementById('chartField').disabled=true;
 chartSamples=[];chartT0=Date.now();
 document.getElementById('chartStatus').innerText='Recording 0s / 45s';
 document.getElementById('chartDl').disabled=true;
 chartTimer=setInterval(chartPoll,500);
 chartPoll();
}
async function chartPoll(){
 if(curTab!=='chart'){chartStop(true);return;}
 let el=(Date.now()-chartT0)/1000;
 if(el>=45||chartSamples.length>=chartMaxPts){chartStop(false);return;}
 try{
  let r=await fetch('/chart?f='+chartField);let j=await r.json();
  chartSamples.push({t:el,v:j.v/1000});
  document.getElementById('chartStatus').innerText='Recording '+el.toFixed(0)+'s / 45s ('+chartSamples.length+' pts)';
  chartDraw();
 }catch(e){}
}
function chartStop(silent){
 if(chartTimer){clearInterval(chartTimer);chartTimer=null;}
 fetch('/chartoff').catch(()=>{});
 if(!silent&&chartSamples.length){document.getElementById('chartStatus').innerText='Done - '+chartSamples.length+' pts recorded';}
 document.getElementById('chartDl').disabled=chartSamples.length===0;
 document.getElementById('chartField').disabled=false;
}
function chartDraw(){
 let c=document.getElementById('chartCv');if(!c||!chartSamples.length)return;
 let g=c.getContext('2d'),W=c.width,H=c.height;
 let vs=chartSamples.map(p=>p.v);
 let mn=Math.min(...vs),mx=Math.max(...vs);
 if(mx-mn<0.001){mx+=0.0005;mn-=0.0005;}
 let pad=(mx-mn)*0.15;mn-=pad;mx+=pad;
 let X=i=>30+(chartSamples[i].t/45)*(W-40);
 let Y=v=>(H-30)-((v-mn)/(mx-mn))*((H-30)-46);
 g.clearRect(0,0,W,H);
 g.strokeStyle='#e2e6ea';g.lineWidth=1;g.fillStyle='#6b7280';g.font='20px sans-serif';
 for(let k=0;k<=2;k++){let v=mn+(mx-mn)*k/2,y=Y(v);g.beginPath();g.moveTo(30,y);g.lineTo(W-10,y);g.stroke();g.fillText(v.toFixed(2),2,y+6);}
 g.strokeStyle='#5a7a6a';g.lineWidth=3;g.beginPath();
 chartSamples.forEach((p,i)=>{i?g.lineTo(X(i),Y(p.v)):g.moveTo(X(i),Y(p.v));});
 g.stroke();
 let last=chartSamples[chartSamples.length-1];
 g.fillStyle='#e8a040';g.beginPath();g.arc(X(chartSamples.length-1),Y(last.v),6,0,6.283);g.fill();
 let m=chartMeta();
 g.fillStyle='#2b2e34';g.font='bold 22px sans-serif';
 g.fillText(m.text+': '+last.v.toFixed(2)+' '+m.unit,30,26);
}
function chartDownload(){
 let m=chartMeta(),csv='time_s,'+m.text+'_'+m.unit+'\n';
 for(let p of chartSamples){csv+=p.t.toFixed(1)+','+p.v.toFixed(3)+'\n';}
 let a=document.createElement('a');
 a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));
 a.download='aobd_'+chartField+'.csv';a.click();
 setTimeout(()=>URL.revokeObjectURL(a.href),2000);
}

// ======================= Offline Log (WROOM only) =========================
// eslint-disable-next-line no-unused-vars
(function(){
 const MODES=['UNKNOWN','S. ECO','ECO','DRIVE','SPORTS','WARP'];
 const COL=['#5a7a6a','#3b7dd8','#d94f4d','#e0a030','#8a5cd8','#0f9b8e'];
 const modeLevel=m=>m===8?1:m===3?2:m===2?3:m===1?4:m===6?5:0;
 const modeName=m=>({1:'SPORTS',2:'DRIVE',3:'ECO',6:'WARP',8:'S. ECO'})[m]||'UNKNOWN';
 const DBN='aobd_log_v2',MAXMEM=3600;              // 1 hour @ 1 Hz
 const $=id=>document.getElementById(id);
 // S = in-memory window for charts: [absSec, volt(0.01V), soc(0.01%), tempC, rangeKm, modeCode, cur(0.1A), rpm, odo(m)]
 let db=null,dbOk=false,ready=false,boot=null,lastSeq=0,S=[],rangeSec=900,polling=false;
 let dropped=0,lastAck=0,retries=0,total=0,writeFail=false;

 function openDB(){
  return new Promise((res,rej)=>{
   if(!window.indexedDB){rej(new Error('IndexedDB not available'));return;}
   const rq=indexedDB.open(DBN,1);
   rq.onupgradeneeded=()=>{const d=rq.result;d.createObjectStore('samples',{keyPath:'id',autoIncrement:true});d.createObjectStore('meta');};
   rq.onsuccess=()=>res(rq.result);
   rq.onerror=()=>rej(rq.error);
   rq.onblocked=()=>rej(new Error('IndexedDB blocked'));
  });
 }
 function txDone(tx){return new Promise((res,rej)=>{tx.oncomplete=()=>res();tx.onerror=()=>rej(tx.error||new Error('tx error'));tx.onabort=()=>rej(tx.error||new Error('tx aborted'));});}
 function mkTx(stores,mode){try{return db.transaction(stores,mode,{durability:'strict'});}catch(e){return db.transaction(stores,mode);}}
 function metaObj(){return {boot:boot,lastSeq:lastSeq,lastAck:lastAck,dropped:dropped};}
 async function saveMeta(){if(!dbOk)return;try{const tx=mkTx(['meta'],'readwrite');tx.objectStore('meta').put(metaObj(),'state');await txDone(tx);}catch(e){}}
 // Atomically store a batch AND the new lastSeq. Resolves only after the data is committed.
 async function commitBatch(rows,meta){const tx=mkTx(['samples','meta'],'readwrite');const st=tx.objectStore('samples');for(const r of rows)st.add(r);tx.objectStore('meta').put(meta,'state');await txDone(tx);}
 async function loadState(){
  db=await openDB();
  const tx=db.transaction(['samples','meta'],'readonly');
  const mreq=tx.objectStore('meta').get('state');
  const creq=tx.objectStore('samples').count();
  const rows=[];
  const cur=tx.objectStore('samples').openCursor(null,'prev');
  cur.onsuccess=()=>{const c=cur.result;if(c&&rows.length<MAXMEM){rows.push(c.value);c.continue();}};
  await txDone(tx);
  const m=mreq.result;
  if(m){boot=m.boot;lastSeq=m.lastSeq>>>0;lastAck=m.lastAck>>>0;dropped=m.dropped||0;}
  total=creq.result||0;
  rows.reverse();
  S=rows.map(r=>[r.t,r.v,r.soc||0,r.temp,r.range,r.mode,r.cur||0,r.rpm||0,r.odo||0]);
 }

 function stat(txt,cls){const e=$('lkstat');if(!e)return;e.innerText=txt;e.className=cls;}
 function linkInfo(){
  $('lkcount').innerText=dbOk?total:S.length;
  $('lkack').innerText=lastAck?('#'+lastAck):'--';
  $('lkdrop').innerText=dropped;
  $('lkretry').innerText=retries;
  const e=$('lkstore');
  e.innerText=!dbOk?'NOT SAVING (memory only)':writeFail?'WRITE FAILING':'IndexedDB \u2713';
  e.className=(dbOk&&!writeFail)?'on':'off';
  const w=$('offWarn');
  if(!dbOk){w.style.display='block';w.innerText='WARNING: phone storage is unavailable (private/blocked browser mode?). Data is only in memory and is lost when this page reloads. The ESP32 is NOT told to delete anything, but it only keeps the last ~1 hour, so older samples will be overwritten. Open this page in a normal (non-private) tab, or download the CSV regularly.';}
  else if(writeFail){w.style.display='block';w.innerText='WARNING: saving to phone storage is failing (storage full or blocked). Nothing is being acknowledged, so the ESP32 keeps the data (last ~1 hour only). Download the CSV and clear phone data to free space.';}
  else w.style.display='none';
 }

 const p2=n=>(n<10?'0':'')+n;
 function fmtFull(t){const d=new Date(t*1000);return d.getFullYear()+'-'+p2(d.getMonth()+1)+'-'+p2(d.getDate())+' '+p2(d.getHours())+':'+p2(d.getMinutes())+':'+p2(d.getSeconds());}
 function csvRow(t,dev,b,seq,v,soc,temp,rg,mode,cur,rpm,odo){return fmtFull(t)+','+t+','+dev+','+b+','+seq+','+(v/100).toFixed(2)+','+(soc/100).toFixed(2)+','+temp+','+rg+','+mode+','+modeName(mode)+','+((cur||0)/10).toFixed(1)+','+rpm+','+(odo/1000).toFixed(1);}
 async function exportCSV(){
  const btn=$('btn_csv');btn.disabled=true;
  try{
   const lines=['timestamp_local,unix_time_s,device_uptime_s,boot_id,seq,voltage_V,soc_percent,motor_temp_C,range_km,mode_code,mode,motor_current_A,motor_rpm,odometer_km'];
   if(dbOk){
    const tx=db.transaction('samples','readonly');
    const rq=tx.objectStore('samples').openCursor();
    rq.onsuccess=()=>{const c=rq.result;if(c){const r=c.value;lines.push(csvRow(r.t,r.dev,r.boot,r.seq,r.v,r.soc,r.temp,r.range,r.mode,r.cur,r.rpm,r.odo));c.continue();}};
    await txDone(tx);
   }else{for(const e of S)lines.push(csvRow(e[0],'','','',e[1],e[2],e[3],e[4],e[5],e[6],e[7],e[8]));}
   if(lines.length<2){alert('No data to export yet.');return;}
   const blob=new Blob(['\uFEFF'+lines.join('\r\n')+'\r\n'],{type:'text/csv;charset=utf-8'});
   const url=URL.createObjectURL(blob);
   const d=new Date();
   const name='aobd_log_'+d.getFullYear()+p2(d.getMonth()+1)+p2(d.getDate())+'_'+p2(d.getHours())+p2(d.getMinutes())+p2(d.getSeconds())+'.csv';
   const a=document.createElement('a');a.href=url;a.download=name;a.style.display='none';document.body.appendChild(a);a.click();
   setTimeout(()=>{URL.revokeObjectURL(url);a.remove();},10000);
  }catch(e){alert('Export failed: '+(e&&e.message?e.message:e));}
  finally{btn.disabled=false;}
 }
 async function clearData(){
  if(!confirm('Delete ALL saved log data from this phone? Download the CSV first if you want to keep it.'))return;
  try{
   if(dbOk){const tx=mkTx(['samples'],'readwrite');tx.objectStore('samples').clear();await txDone(tx);}
   S=[];total=0;dropped=0;retries=0;
   await saveMeta();   // keeps boot/lastSeq so already-acknowledged samples are not reported as lost
   linkInfo();render();
  }catch(e){alert('Clear failed: '+(e&&e.message?e.message:e));}
 }

 async function doAck(){
  const aj=await fj('/ack?seq='+lastSeq+'&boot='+boot);
  if(aj.ok&&aj.acked>=lastSeq){lastAck=aj.acked;await saveMeta();return true;}
  return false;
 }
 async function fj(u,ms){const c=new AbortController();const to=setTimeout(()=>c.abort(),ms||6000);try{const r=await fetch(u,{signal:c.signal,cache:'no-store'});return await r.json();}finally{clearTimeout(to);}}

 async function poll(){
  if(polling||!ready)return;
  polling=true;let again=false;
  try{
   if(dbOk&&boot!==null&&lastSeq>lastAck){try{await doAck();}catch(e){}}
   const j=await fj('/log?since='+lastSeq,8000);
   if(boot!==null&&j.boot!==boot){
    boot=j.boot;lastSeq=0;lastAck=0;await saveMeta();
    stat('Device restarted - resyncing','off');again=true;
   }else{
    boot=j.boot;
    const s=j.s;let cs=0,ok=(s.length===j.n);
    for(let i=0;i<s.length;i++){
     const e=s[i];
     cs=(cs+e[0]+e[1]+e[2]+e[3]+(e[4]&0xFFFF)+e[5]+e[6]+(e[7]&0xFFFF)+e[8]+e[9])>>>0;
     if(i>0&&e[0]!==s[i-1][0]+1)ok=false;
    }
    if(ok&&cs!==(j.cs>>>0))ok=false;
    if(ok&&j.n>0&&(s[0][0]!==j.first||s[s.length-1][0]!==j.last))ok=false;
    if(!ok){retries++;stat('Checksum mismatch - retrying','off');}
    else if(j.n>0){
     const nowAbs=Math.floor(Date.now()/1000);
     let prev=S.length?S[S.length-1][0]:0;
     const rows=[],mem=[];
     for(let i=0;i<s.length;i++){
      const e=s[i];
      let a=nowAbs-(j.now-e[1]);if(a<prev)a=prev;prev=a;
      rows.push({t:a,dev:e[1],boot:j.boot,seq:e[0],v:e[2],soc:e[3],temp:e[4],range:e[5],mode:e[6],cur:e[7],rpm:e[8],odo:e[9]});
      mem.push([a,e[2],e[3],e[4],e[5],e[6],e[7],e[8],e[9]]);
     }
     // Lost samples counted ONCE from the real seq gap; lastSeq and the total commit together.
     const gap=Math.max(0,s[0][0]-lastSeq-1);
     const newDropped=dropped+gap;
     let saved=true;
     if(dbOk){try{await commitBatch(rows,{boot:j.boot,lastSeq:j.last,lastAck:lastAck,dropped:newDropped});}catch(e){saved=false;}}
     writeFail=!saved;
     if(!saved){stat('Phone storage failed - NOT acknowledged, retrying','off');}
     else{
      for(const m of mem)S.push(m);
      if(S.length>MAXMEM)S.splice(0,S.length-MAXMEM);
      total+=rows.length;lastSeq=j.last;dropped=newDropped;
      if(dbOk){let acked=false;try{acked=await doAck();}catch(e){}stat(acked?'Delivered & verified \u2713':'Saved on phone - ack pending','off');if(acked)$('lkstat').className='on';}
      else{stat('WARNING: memory only - NOT saved, NOT acknowledged','off');}
      if(j.n>=j.max)again=true;
     }
    }else{
     if(j.oldest-1>lastSeq){dropped+=j.oldest-1-lastSeq;lastSeq=j.oldest-1;await saveMeta();}
     stat((dbOk?lastAck:lastSeq)?'Up to date \u2713':'Waiting for vehicle data...',(dbOk?lastAck:lastSeq)?'on':'off');
    }
   }
   linkInfo();render();
  }catch(e){stat('Link error - retrying','off');}
  finally{polling=false;if(again)setTimeout(poll,50);}
 }

 function fmtClock(t,sec){const d=new Date(t*1000);return p2(d.getHours())+':'+p2(d.getMinutes())+(sec?':'+p2(d.getSeconds()):'');}
 // Overlays several lines in one chart. Each line is scaled to its own min/max, so
 // signals with different units can share a chart. A legend with Now/Min/Max is
 // written into the chart's stat line.
 function offChart(id,series,opt){
  opt=opt||{};
  const cv=$(id);if(!cv)return;
  const dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
  if(!w||!h)return;
  cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);
  const c=cv.getContext('2d');c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
  c.font='11px Arial';
  const L=opt.levels?62:44,R=14,T=10,B=20,pw=w-L-R,ph=h-T-B;
  let t0=Infinity,t1=-Infinity,any=false;
  for(const s of series)for(const p of s.pts){any=true;if(p[0]<t0)t0=p[0];if(p[0]>t1)t1=p[0];}
  if(!any){c.fillStyle='#9aa0a8';c.textAlign='center';c.textBaseline='middle';c.fillText('Waiting for data...',w/2,h/2);return;}
  if(t1-t0<10)t0=t1-10;
  for(const s of series){
   if(opt.levels){s.ymin=-0.5;s.ymax=opt.levels.length-0.5;continue;}
   let lo=Infinity,hi=-Infinity;
   for(const p of s.pts){if(p[1]<lo)lo=p[1];if(p[1]>hi)hi=p[1];}
   if(!isFinite(lo)){lo=0;hi=1;}
   const ms=s.minSpan||0.001;
   if(hi-lo<ms){const m=(hi+lo)/2;lo=m-ms/2;hi=m+ms/2;}
   const pad=(hi-lo)*0.1;s.ymin=lo-pad;s.ymax=hi+pad;if(s.floor0&&s.ymin<0)s.ymin=0;
  }
  const X=t=>L+(t-t0)/(t1-t0)*pw,Y=(s,v)=>T+(1-(v-s.ymin)/(s.ymax-s.ymin))*ph;
  c.strokeStyle='#e2e6ea';c.fillStyle='#6b7280';c.lineWidth=1;
  if(opt.levels){c.textAlign='right';c.textBaseline='middle';for(let i=0;i<opt.levels.length;i++){const y=Math.round(Y(series[0],i))+0.5;c.beginPath();c.moveTo(L,y);c.lineTo(L+pw,y);c.stroke();c.fillText(opt.levels[i],L-6,y);}}
  else for(let i=0;i<=4;i++){const y=Math.round(T+ph*i/4)+0.5;c.beginPath();c.moveTo(L,y);c.lineTo(L+pw,y);c.stroke();}
  c.textBaseline='top';
  for(let i=0;i<=4;i++){c.textAlign=i===0?'left':i===4?'right':'center';c.fillText(fmtClock(t0+(t1-t0)*i/4,(t1-t0)<=900),L+pw*i/4,T+ph+6);}
  for(const s of series){
   c.strokeStyle=s.color;c.lineWidth=2;c.lineJoin='round';c.beginPath();
   let prev=null;
   for(const p of s.pts){const x=X(p[0]),y=Y(s,p[1]);if(prev===null||p[0]-prev[0]>10)c.moveTo(x,y);else if(opt.step){c.lineTo(x,Y(s,prev[1]));c.lineTo(x,y);}else c.lineTo(x,y);prev=p;}
   c.stroke();
   const lp=s.pts[s.pts.length-1];
   if(lp){c.fillStyle=s.color;c.beginPath();c.arc(X(lp[0]),Y(s,lp[1]),3.5,0,Math.PI*2);c.fill();}
  }
 }
 function seriesStat(id,series){
  const el=$(id);if(!el)return;
  if(!series.length||!series[0].pts.length){el.innerText='--';return;}
  el.innerHTML=series.map(s=>{let lo=Infinity,hi=-Infinity;for(const p of s.pts){if(p[1]<lo)lo=p[1];if(p[1]>hi)hi=p[1];}const nw=s.pts[s.pts.length-1][1];
   return '<span style="color:'+s.color+';font-weight:700">'+s.label+'</span> '+nw.toFixed(s.dec)+s.unit+' <span style="color:#9aa0a8">[min '+lo.toFixed(s.dec)+', max '+hi.toFixed(s.dec)+']</span>';}).join(' &nbsp;|&nbsp; ');
 }
 function render(){
  if(curTab!=='chart'||curSub!=='off')return;
  const n=S.length;let i=0;
  if(n){const from=S[n-1][0]-rangeSec;i=n-1;while(i>0&&S[i-1][0]>=from)i--;}
  const col=k=>{const a=[];for(let j=i;j<n;j++)a.push([S[j][0],S[j][k]]);return a;};
  const bat=[{label:'Voltage',unit:' V',dec:2,minSpan:1,color:COL[1],pts:col(1).map(p=>[p[0],p[1]/100])},
             {label:'SoC',unit:' %',dec:2,minSpan:5,color:COL[0],pts:col(2).map(p=>[p[0],p[1]/100])}];
  const mot=[{label:'Motor Temp',unit:' \u00B0C',dec:0,minSpan:10,color:COL[2],pts:col(3)},
             {label:'Motor Current',unit:' A',dec:1,minSpan:10,color:COL[3],pts:col(6).map(p=>[p[0],(p[1]||0)/10])},
             {label:'Motor RPM',unit:' rpm',dec:0,minSpan:500,floor0:true,color:COL[4],pts:col(7)}];
  const dist=[{label:'Range',unit:' km',dec:0,minSpan:10,floor0:true,color:COL[0],pts:col(4)},
              {label:'Odometer',unit:' km',dec:1,minSpan:1,floor0:true,color:COL[5],pts:col(8).map(p=>[p[0],p[1]/1000])}];
  const mode=[{label:'Ride Mode',unit:'',dec:0,color:COL[5],pts:col(5).map(p=>[p[0],modeLevel(p[1])])}];
  offChart('ch_bat',bat,{});
  offChart('ch_mot',mot,{});
  offChart('ch_dist',dist,{});
  offChart('ch_mode',mode,{levels:MODES,step:true});
  seriesStat('st_bat',bat);
  seriesStat('st_mot',mot);
  seriesStat('st_dist',dist);
  const mp=mode[0].pts;
  if(mp.length){let ch=0;for(let j=1;j<mp.length;j++)if(mp[j][1]!==mp[j-1][1])ch++;$('st_mode').innerHTML='<span style="color:'+COL[5]+';font-weight:700">Ride Mode</span> '+MODES[mp[mp.length-1][1]]+' &nbsp;\u00B7&nbsp; Mode changes '+ch;}
  else $('st_mode').innerText='--';
 }

 window.offOpen=()=>{linkInfo();poll();render();};
 window.offRange=function(sec){rangeSec=sec;[300,900,3600].forEach(k=>$('rb_'+k).classList.toggle('active',k===sec));render();};
 window.offExportCSV=exportCSV;
 window.offClearData=clearData;
 window.addEventListener('resize',render);
 document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});

 // Open storage first, then collect in the background on EVERY tab
 async function init(){
  try{await loadState();dbOk=true;}catch(e){dbOk=false;db=null;}
  ready=true;
  linkInfo();
  stat(dbOk?'Starting...':'WARNING: phone storage unavailable - memory only','off');
  poll();
  setInterval(poll,4500);
 }
 if(document.readyState==='loading') document.addEventListener('DOMContentLoaded',init); else init();
})();
setInterval(updateAll,1000);
window.onload=function(){showTab('dash');};
</script></head><body>
<header><h1>&#9889; A OBD Scanner v5</h1><p style="margin:6px 0 0;color:var(--muted);font-size:13px;">Developed by <b style="color:var(--text);">Community</b> and <b style="color:var(--text);">sam0_0</b> &bull; A OBD Scanner</p><p style="margin:10px 0 0;"><a href="https://www.buymeacoffee.com/sam0_0" target="_blank" style="display:inline-block;background:#ffdd00;color:#1a1a1a;padding:7px 16px;border-radius:999px;font-size:13px;font-weight:700;text-decoration:none;box-shadow:0 2px 6px rgba(0,0,0,0.12);">&#9749; Support this project &mdash; Buy Me a Coffee</a></p></header>
<div class="tabs">
<button class="tab active" id="tb_dash" onclick="showTab('dash')">Dashboard</button>
<button class="tab" id="tb_cells" onclick="showTab('cells')">Cell Info</button>
<button class="tab" id="tb_charger" onclick="showTab('charger')">Charger</button>
<button class="tab" id="tb_extra" onclick="showTab('extra')">Extra</button>
<button class="tab" id="tb_others" onclick="showTab('others')">Others</button>
<button class="tab" id="tb_chart" onclick="showTab('chart')">Chart</button>
</div>

<div id="dash" class="panel active">
<div class="card"><h2>Battery &amp; Drive</h2>
<table>
<tr><td>SoC <span style="font-size:11px;color:var(--muted)">[actual, without buffer]</span></td><td class='v' id='soc'>--</td></tr>
<tr><td>SoH</td><td class='v' id='soh'>--</td></tr>
<tr><td>Delta SoC</td><td class='v' id='bdelta'>--</td></tr>
<tr><td>Voltage</td><td class='v' id='volt'>--</td></tr>
<tr><td>Voltage Imbalance</td><td class='v' id='imb'>--</td></tr>
<tr><td>Drive Mode</td><td class='v' id='mode'>--</td></tr>
<tr><td>Range</td><td class='v' id='range'>--</td></tr>
</table></div>
<div class="card"><h2>Switches &amp; Inputs</h2>
<table>
<tr><td>Key</td><td class='off' id='key'>--</td></tr>
<tr><td>Start Switch</td><td class='off' id='start'>--</td></tr>
<tr><td>Front Brake</td><td class='off' id='fbrake'>--</td></tr>
<tr><td>Rear Brake</td><td class='off' id='rbrake'>--</td></tr>
<tr><td>High Beam</td><td class='off' id='hbeam'>--</td></tr>
<tr><td>Horn</td><td class='off' id='horn'>--</td></tr>
<tr><td>Indicator Left</td><td class='off' id='indl'>--</td></tr>
<tr><td>Indicator Right</td><td class='off' id='indr'>--</td></tr>
<tr><td>Indicator Center</td><td class='off' id='indc'>--</td></tr>
<tr><td>Kill Switch</td><td class='off' id='kill'>--</td></tr>
<tr><td>Storage Switch</td><td class='off' id='storage'>--</td></tr>
<tr><td>Sidestand</td><td class='off' id='stand'>--</td></tr>
<tr><td>Seat Lock</td><td class='off' id='seatlock'>--</td></tr>
</table></div>
</div>

<div id="cells" class="panel">
<div class="card"><h2>Cell SoH Range</h2>
<table>
<tr><td>Maximum Cell SoH</td><td class='v' id='cmax'>--</td></tr>
<tr><td>Minimum Cell SoH</td><td class='v' id='cmin'>--</td></tr>
</table></div>
<div class="card ctable"><h2>Cells (14S)</h2>
<table>
<thead><tr><th>Cell</th><th>SoH</th><th>SoC</th><th>Voltage</th></tr></thead>
<tbody id="celltable"></tbody></table></div>
</div>

<div id="charger" class="panel">
<div class="card"><h2>Charger</h2>
<table>
<tr><td>Output Voltage</td><td class='v' id='coutv'>--</td></tr>
<tr><td>Output Current</td><td class='v' id='couti'>--</td></tr>
<tr><td>Input Voltage</td><td class='v' id='cinv'>--</td></tr>
<tr><td>Power [calculated]</td><td class='v' id='cwatts'>--</td></tr>
<tr><td>Balancing State</td><td class='v' id='bal'>--</td></tr>
<tr><td>Charger Connected</td><td class='off' id='cconnected'>--</td></tr>
<tr><td>Fan Speed 1</td><td class='v' id='cfan1'>--</td></tr>
<tr><td>Fan Speed 2</td><td class='v' id='cfan2'>--</td></tr>
<tr><td>Charger Lock 1</td><td class='off' id='clock1'>--</td></tr>
<tr><td>Charger Lock 2</td><td class='off' id='clock2'>--</td></tr>
</table></div>
</div>

<div id="extra" class="panel">
<div class="card"><h2>Temperatures</h2>
<table>
<tr><td>Cell Temp 1</td><td class='v' id='xct0'>--</td></tr>
<tr><td>Cell Temp 2</td><td class='v' id='xct1'>--</td></tr>
<tr><td>Cell Temp 3</td><td class='v' id='xct2'>--</td></tr>
<tr><td>Cell Temp 4</td><td class='v' id='xct3'>--</td></tr>
<tr><td>Cell Temp 5 <span style="font-size:11px;color:var(--muted)">[unused on NMC]</span></td><td class='v' id='xct4'>--</td></tr>
<tr><td>AFE Die Temp</td><td class='v' id='xdie'>--</td></tr>
<tr><td>FET Temp <span style="font-size:11px;color:var(--muted)">[probable]</span></td><td class='v' id='xeft'>--</td></tr>
<tr><td>ECI Temp <span style="font-size:11px;color:var(--muted)">[probable]</span></td><td class='v' id='xeci'>--</td></tr>
<tr><td>Sense Resistor Temp <span style="font-size:11px;color:var(--muted)">[probable]</span></td><td class='v' id='xsr'>--</td></tr>
</table></div>
<div class="card"><h2>Battery (Additional)</h2>
<table>
<tr><td>Pack Current</td><td class='v' id='xci'>--</td></tr>
<tr><td>State of Energy</td><td class='v' id='xsoe'>--</td></tr>
<tr><td>SoC (EKF)</td><td class='v' id='xsekf'>--</td></tr>
<tr><td>Max SoC</td><td class='v' id='xmsoc'>--</td></tr>
<tr><td>Min SoC Est</td><td class='v' id='xminsoc'>--</td></tr>
<tr><td>Min V1 Est</td><td class='v' id='xv1'>--</td></tr>
<tr><td>Min V2 Est</td><td class='v' id='xv2'>--</td></tr>
</table></div>
<div class="card"><h2>Current Limits</h2>
<table>
<tr><td>Derated Discharge Current</td><td class='v' id='xdd'>--</td></tr>
<tr><td>Derated Charge Current</td><td class='v' id='xdc'>--</td></tr>
<tr><td>Derated Regen Current</td><td class='v' id='xdr'>--</td></tr>
<tr><td>Discharge Limit (Temp)</td><td class='v' id='xdl'>--</td></tr>
<tr><td>ChgAlg I CellRule</td><td class='v' id='xcr'>--</td></tr>
<tr><td>ChgAlg I Cmd Rate Limited</td><td class='v' id='xcrl'>--</td></tr>
<tr><td>ChgAlg I Post Quantizer</td><td class='v' id='xcpq'>--</td></tr>
</table></div>
<div class="card"><h2>Vehicle &amp; Firmware</h2>
<table>
<tr><td>Odometer</td><td class='v' id='xodo'>--</td></tr>
<tr><td>BMS HW Version</td><td class='v' id='xbhw'>--</td></tr>
<tr><td>BMS FW</td><td class='v' id='xbfw'>--</td></tr>
<tr><td>BCM FW</td><td class='v' id='xcfw'>--</td></tr>
<tr><td>Gateway FW</td><td class='v' id='xgfw'>--</td></tr>
<tr><td>Motor FW</td><td class='v' id='xmfw'>--</td></tr>
<tr><td>Battery BIN</td><td class='v' id='xbin'>--</td></tr>
</table></div>
</div>

<div id="others" class="panel">
<div class="card"><h2>Drive Modes</h2>
<table>
<tr><td>Forced Eco</td><td class='off' id='feco'>--</td></tr>
<tr><td>Smart Eco</td><td class='off' id='seco'>--</td></tr>
</table></div>
<div class="card"><h2>Locks &amp; Motor</h2>
<table>
<tr><td>Motor RPM</td><td class='v' id='rpm'>--</td></tr>
<tr><td>Motor Battery Current</td><td class='v' id='mbi'>--</td></tr>
<tr><td>Motor Temperature</td><td class='v' id='mt'>--</td></tr>
</table></div>
<div class="card"><h2>Controller &amp; Throttle</h2>
<table>
<tr><td>Controller Temp</td><td class='v' id='ctemp'>--</td></tr>
<tr><td>Throttle Out</td><td class='v' id='thro'>--</td></tr>
<tr><td>Throttle 1</td><td class='v' id='thr1'>--</td></tr>
<tr><td>Throttle 3</td><td class='v' id='thr3'>--</td></tr>
<tr><td>PDU Voltage</td><td class='v' id='pdu'>--</td></tr>
</table></div>
<div class="card"><h2>Environment</h2>
<table>
<tr><td>Display Fan</td><td class='off' id='dfan'>--</td></tr>
<tr><td>Humidity</td><td class='v' id='hum'>--</td></tr>
<tr><td>Ambient Temp</td><td class='v' id='amb'>--</td></tr>
</table></div>
</div>

<div id="chart" class="panel">
<div class="subtab">
<button class="active" id="sub_live" onclick="showSub('live')">Live Chart &mdash; 45s @ 2/s</button>
<button id="sub_off" onclick="showSub('off')">Offline Log</button>
</div>

<div id="subLive">
<div class="card"><h2>Live Chart &mdash; 45s @ 2/s</h2>
<table><tr><td>Signal</td><td><select id="chartField" style="background:var(--card);color:var(--text);border:1px solid var(--border);border-radius:8px;padding:8px;font-size:15px;max-width:100%;">
<option value="rpm" data-unit="rpm">Motor RPM</option>
<option value="mt" data-unit="°C">Motor Temp</option>
<option value="ctemp" data-unit="°C">Controller Temp</option>
<option value="mbi" data-unit="A">Motor Current</option>
<option value="soc" data-unit="%">SoC</option>
<option value="volt" data-unit="V">Battery Voltage</option>
<option value="coutv" data-unit="V">Charger Out V</option>
<option value="couti" data-unit="A">Charger Out I</option>
<option value="watts" data-unit="W">Charger Power</option>
<option value="cinv" data-unit="V">Charger In V</option>
<option value="pdu" data-unit="V">PDU Voltage</option>
<option value="thro" data-unit="%">Throttle Out</option>
<option value="thr1" data-unit="%">Throttle 1</option>
<option value="thr3" data-unit="%">Throttle 3</option>
<option value="hum" data-unit="%">Humidity</option>
<option value="amb" data-unit="°C">Ambient Temp</option>
<option value="maxS" data-unit="%">Max Cell SoH</option>
<option value="minS" data-unit="%">Min Cell SoH</option>
<option value="soh" data-unit="%">SoH</option>
<option value="fan1" data-unit="rpm">Charger Fan 1</option>
<option value="fan2" data-unit="rpm">Charger Fan 2</option>
<option value="range" data-unit="km">Range</option>
<option value="imb" data-unit="V">Imbalance</option>
<option value="bdelta" data-unit="%">Delta SoC</option>
<option value="bal" data-unit="-">Balancing</option>
<option value="ct0" data-unit="°C">Cell Temp 1</option>
<option value="ct1" data-unit="°C">Cell Temp 2</option>
<option value="ct2" data-unit="°C">Cell Temp 3</option>
<option value="ct3" data-unit="°C">Cell Temp 4</option>
<option value="ct4" data-unit="°C">Cell Temp 5</option>
<option value="die" data-unit="°C">AFE Die Temp</option>
<option value="eft" data-unit="°C">FET Temp</option>
<option value="eci" data-unit="°C">ECI Temp</option>
<option value="sr" data-unit="°C">Sense Resistor Temp</option>
<option value="ci" data-unit="A">Pack Current</option>
<option value="dd" data-unit="A">Derated Discharge I</option>
<option value="dc" data-unit="A">Derated Charge I</option>
<option value="dr" data-unit="A">Derated Regen I</option>
<option value="dl" data-unit="A">Discharge Limit (Temp)</option>
<option value="cr" data-unit="A">ChgAlg I CellRule</option>
<option value="crl" data-unit="A">ChgAlg I CmdRateLtd</option>
<option value="cpq" data-unit="A">ChgAlg I PostQuant</option>
<option value="soe" data-unit="%">State of Energy</option>
<option value="sekf" data-unit="%">SoC (EKF)</option>
<option value="msoc" data-unit="%">Max SoC</option>
<option value="minsoc" data-unit="%">Min SoC Est</option>
<option value="v1" data-unit="V">Min V1 Est</option>
<option value="v2" data-unit="V">Min V2 Est</option>
<option value="odo" data-unit="km">Odometer</option>
</select></td></tr></table>
<div style="display:flex;gap:10px;padding:12px 16px;flex-wrap:wrap;">
<button class="tab" style="max-width:none;flex:none;padding:10px 22px;" onclick="chartStart()">Start</button>
<button class="tab" style="max-width:none;flex:none;padding:10px 22px;" onclick="chartStop(false)">Stop</button>
<button class="tab" style="max-width:none;flex:none;padding:10px 22px;" id="chartDl" onclick="chartDownload()" disabled>CSV</button>
</div>
<p id="chartStatus" style="padding:0 16px;color:var(--muted);font-size:14px;">Idle &mdash; pick a signal and press Start. Other readings pause while charting.</p>
<canvas id="chartCv" width="600" height="280" style="width:100%;display:block;"></canvas>
</div>
</div>

<div id="subOff" style="display:none">
<div id="offWarn" class="card" style="display:none;background:#fdecea;border-color:#d94f4f;color:#8a1c1c;padding:12px 16px;font-size:13px;font-weight:600;line-height:1.4;"></div>
<div class="card"><h2>Offline Logs</h2>
<table>
<tr><td>Delivery</td><td class='off' id='lkstat'>--</td></tr>
<tr><td>Phone storage</td><td class='off' id='lkstore'>--</td></tr>
<tr><td>Samples stored on phone</td><td class='v' id='lkcount'>0</td></tr>
<tr><td>Last verified sample</td><td class='v' id='lkack'>--</td></tr>
<tr><td>Not recoverable</td><td class='v' id='lkdrop'>0</td></tr>
<tr><td>Checksum retries</td><td class='v' id='lkretry'>0</td></tr>
</table></div>
<div class="card"><div style="display:flex;gap:10px;padding:12px 16px;flex-wrap:wrap;">
<button class="tab offbtn" id="btn_csv" onclick="offExportCSV()">Download CSV</button>
<button class="tab offbtn" onclick="offClearData()">Clear phone data</button>
</div></div>
<div class="card"><div style="display:flex;gap:10px;padding:12px 16px;flex-wrap:wrap;">
<button class="tab offbtn" id="rb_300" onclick="offRange(300)">5 min</button>
<button class="tab offbtn" id="rb_900" onclick="offRange(900)">15 min</button>
<button class="tab offbtn" id="rb_3600" onclick="offRange(3600)">1 hour</button>
</div></div>
<div class="card"><h2>Battery &mdash; Voltage / SoC</h2><div class="cs" style="padding:6px 16px;color:var(--muted);font-size:13px;" id="st_bat">--</div><canvas class="chart" id="ch_bat" width="600" height="240" style="width:100%;display:block;"></canvas></div>
<div class="card"><h2>Motor &mdash; Temp / Current / RPM</h2><div class="cs" style="padding:6px 16px;color:var(--muted);font-size:13px;" id="st_mot">--</div><canvas class="chart" id="ch_mot" width="600" height="240" style="width:100%;display:block;"></canvas></div>
<div class="card"><h2>Distance &mdash; Range / Odometer</h2><div class="cs" style="padding:6px 16px;color:var(--muted);font-size:13px;" id="st_dist">--</div><canvas class="chart" id="ch_dist" width="600" height="240" style="width:100%;display:block;"></canvas></div>
<div class="card"><h2>Ride Mode</h2><div class="cs" style="padding:6px 16px;color:var(--muted);font-size:13px;" id="st_mode">--</div><canvas class="chart" id="ch_mode" width="600" height="240" style="width:100%;display:block;"></canvas></div>
</div>
</div>

<script>
(function(){
  let ct='';
  for(let i=0;i<14;i++){
    ct+='<tr><td>C'+(i+1)+'</td><td class="cv" id="sh'+i+'">--</td><td class="cv" id="sc'+i+'">--</td><td class="cv" id="cv'+i+'">--</td></tr>';
  }
  document.getElementById('celltable').innerHTML=ct;
})();
</script>
</body></html>
)HTML";

void handleRoot(){ server.send_P(200, "text/html", htmlPage); }
char jsonBuf[512];
bool fillData(const Telemetry &t){
  const int32_t amb=t.ambientTemp, ambMag=amb<0?-amb:amb;
  const char *ambSign=amb<0?"-":"";
  int n=snprintf(jsonBuf,sizeof(jsonBuf),
    "{\"soc\":%d.%02d,\"soh\":%d.%02d,\"bdelta\":%d.%02d,\"volt\":%d.%02d,\"imb\":%d.%04d,\"rpm\":%d,\"mode\":\"%s\",\"range\":%d,\"fbrake\":%s,\"rbrake\":%s,\"hbeam\":%s,\"start\":%s,\"kill\":%s,\"storage\":%s,\"horn\":%s,\"indl\":%s,\"indr\":%s,\"indc\":%s,\"key\":%s,\"stand\":%s,\"dfan\":%s,\"feco\":%s,\"seco\":%s,\"hum\":%d.%02d,\"amb\":%s%d.%02d,\"seat\":%s}",
    t.soc/100,abs(t.soc%100), t.soh/100,abs(t.soh%100), t.deltaSoc/100,abs(t.deltaSoc%100),
    t.voltage/100,abs(t.voltage%100), t.imbalance/10000,abs(t.imbalance%10000),
    t.rpm,driveModeName(t.driveMode),t.rangeKm,
    t.frontBrake?"true":"false",t.rearBrake?"true":"false",t.highBeam?"true":"false",t.startSwitch?"true":"false",
    t.killSwitch?"true":"false",t.storageSwitch?"true":"false",t.horn?"true":"false",
    t.indicatorLeftSide?"true":"false",t.indicatorRightSide?"true":"false",t.indicatorCenterSide?"true":"false",t.keyOn?"true":"false",t.sideStand?"true":"false",
    t.displayFan?"true":"false",t.forcedEco?"true":"false",t.smartEco?"true":"false",
    t.humidity/100,abs(t.humidity%100),ambSign,(int)(ambMag/100),abs((int)(ambMag%100)),t.seatLock?"true":"false");
  return !(n<0||n>=(int)sizeof(jsonBuf));
}
void handleData(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  if(!fillData(t)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",jsonBuf);
}

char cellBuf[640];
bool fillCells(const Telemetry &t){
  char *p=cellBuf; int rem=sizeof(cellBuf);
  int n=snprintf(p,rem,"{\"v\":["); if(n<0||n>=rem)return false; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d.%04d%s",t.cellV[i]/10000,abs(t.cellV[i]%10000),i<13?",":""); if(n<0||n>=rem)return false; p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"h\":["); if(n<0||n>=rem)return false; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d%s",t.cellSoh[i],i<13?",":""); if(n<0||n>=rem)return false; p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"s\":["); if(n<0||n>=rem)return false; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%u%s",t.cellSoc[i],i<13?",":""); if(n<0||n>=rem)return false; p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"maxS\":%d.%02d,\"minS\":%d.%02d}",t.maxCellSoh/100,abs(t.maxCellSoh%100),t.minCellSoh/100,abs(t.minCellSoh%100)); if(n<0||n>=rem)return false;
  return true;
}
void handleCells(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  if(!fillCells(t)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",cellBuf);
}

char chargerBuf[384];
bool fillCharger(const Telemetry &t){
  const int32_t motorI=t.motorI;
  const int32_t motorMag=motorI<0?-motorI:motorI;
  const char *motorSign=motorI<0?"-":"";
  const uint64_t powerProduct=(uint64_t)t.chargerOV*(uint64_t)t.chargerOI;
  const uint32_t wattsWhole=(uint32_t)(powerProduct/1000000ULL);
  const unsigned wattsFrac=(unsigned)((powerProduct%1000000ULL)/10000ULL);
  int n=snprintf(chargerBuf,sizeof(chargerBuf),
    "{\"coutv\":%d.%03d,\"couti\":%d.%03d,\"cinv\":%d.%03d,\"mbi\":%s%d.%01d,\"mt\":%d,\"bal\":%d,\"watts\":%u.%02u,\"fan1\":%d,\"fan2\":%d,\"cconnected\":%s,\"seat\":%s,\"clock1\":%s,\"clock2\":%s,\"ctemp\":%d,\"thro\":%d,\"thr1\":%d,\"thr3\":%d,\"pdu\":%d.%03d}",
    (int)(t.chargerOV/1000),abs((int)(t.chargerOV%1000)),
    (int)(t.chargerOI/1000),abs((int)(t.chargerOI%1000)),
    (int)(t.chargerIV/1000),abs((int)(t.chargerIV%1000)),
    motorSign,(int)(motorMag/10),abs((int)(motorMag%10)),
    (int)t.motorTemp, t.balancing, wattsWhole, wattsFrac,
    t.chargerFan1, t.chargerFan2,
    t.chargerConnected?"true":"false",t.seatLock?"true":"false",t.chargerLock1?"true":"false",t.chargerLock2?"true":"false",
    (int)t.controllerTemp,(int)t.throttleOut,(int)t.throttle1,(int)t.throttle3,(int)(t.pduVoltage/1000),abs((int)(t.pduVoltage%1000)));
  if(n<0||n>=(int)sizeof(chargerBuf))return false;
  return true;
}
void handleCharger(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  if(!fillCharger(t)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",chargerBuf);
}

char extraBuf[1024];
#define EXTRA_APPEND(...) do{ n=snprintf(p,rem,__VA_ARGS__); if(n<0||n>=rem)return false; p+=n; rem-=n; }while(0)
bool fillExtra(const Telemetry &t){
  char bin[16]; int bi=0;
  for(int i=0;i<14;i++){ char c=t.binId[i]; if(!c)break; bin[bi++]=((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z'))?c:'?'; }
  bin[bi]=0;
  char *p=extraBuf; int rem=sizeof(extraBuf); int n;
  EXTRA_APPEND("{\"ct\":[%d,%d,%d,%d,%d],\"die\":%d,\"ci\":%d,\"eft\":%d,\"eci\":%d,\"sr\":%d,",
    t.cellTemp[0],t.cellTemp[1],t.cellTemp[2],t.cellTemp[3],t.cellTemp[4],
    t.afeDieTemp,t.packCurrent,t.eftTemp,t.eciTemp,t.senseTemp);
  EXTRA_APPEND("\"dd\":%u,\"dc\":%u,\"dr\":%u,\"dl\":%u,\"cr\":%u,\"crl\":%u,\"cpq\":%d,",
    t.deratedDsgI,t.deratedChgI,t.deratedRegenI,t.dsgLimTemp,t.chgCellRule,t.chgCmdRateLtd,t.chgPostQuant);
  EXTRA_APPEND("\"soe\":%u,\"sekf\":%u,\"msoc\":%u,\"minsoc\":%u,\"v1\":%d,\"v2\":%d,",
    t.soe,t.socEkf,t.maxSoc,t.minSocEst,t.minV1Est,t.minV2Est);
  EXTRA_APPEND("\"odo\":%lu,\"bhw\":%u,\"bfw\":[%u,%u,%u],\"cfw\":[%u,%u,%u],\"gfw\":[%u,%u,%u],\"mfw\":[%u,%u,%u],\"bin\":\"%s\"}",
    (unsigned long)t.odometer,t.bmsHwVer,
    t.bmsFw[0],t.bmsFw[1],t.bmsFw[2],t.bcmFw[0],t.bcmFw[1],t.bcmFw[2],
    t.gtwFw[0],t.gtwFw[1],t.gtwFw[2],t.motorFw[0],t.motorFw[1],t.motorFw[2],bin);
  return true;
}
void handleExtra(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  if(!fillExtra(t)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",extraBuf);
}
void handleAll(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  if(!fillData(t)||!fillCells(t)||!fillCharger(t)||!fillExtra(t)){server.send(500,"application/json","{}");return;}
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json","");
  server.sendContent("{\"d\":"); server.sendContent(jsonBuf);
  server.sendContent(",\"c\":"); server.sendContent(cellBuf);
  server.sendContent(",\"ch\":"); server.sendContent(chargerBuf);
  server.sendContent(",\"x\":"); server.sendContent(extraBuf);
  server.sendContent("}");
  server.sendContent("");
}

char chartBuf[64];
uint16_t chartIdFor(const char *f){
  if(!strcmp(f,"soc")||!strcmp(f,"soh")||!strcmp(f,"bal")) return 0x131;
  if(!strcmp(f,"bdelta")) return 0x13D;
  if(!strcmp(f,"volt")||!strcmp(f,"imb")) return 0x141;
  if(!strcmp(f,"rpm")) return 0x510;
  if(!strcmp(f,"range")) return 0x200;
  if(!strcmp(f,"coutv")||!strcmp(f,"couti")||!strcmp(f,"watts")) return 0x201;
  if(!strcmp(f,"cinv")) return 0x203;
  if(!strcmp(f,"mbi")||!strcmp(f,"mt")||!strcmp(f,"ctemp")) return 0x18c;
  if(!strcmp(f,"thro")||!strcmp(f,"thr1")||!strcmp(f,"thr3")) return 0x38c;
  if(!strcmp(f,"hum")||!strcmp(f,"amb")) return 0x140;
  if(!strcmp(f,"maxS")||!strcmp(f,"minS")) return 0x14D;
  if(!strcmp(f,"pdu")) return 0x102;
  if(!strcmp(f,"fan1")||!strcmp(f,"fan2")) return 0x208;
  if(!strcmp(f,"ct0")||!strcmp(f,"ct1")||!strcmp(f,"ct2")||!strcmp(f,"ct3")) return 0x136;
  if(!strcmp(f,"ct4")||!strcmp(f,"eft")||!strcmp(f,"eci")||!strcmp(f,"sr")) return 0x137;
  if(!strcmp(f,"die")||!strcmp(f,"ci")) return 0x135;
  if(!strcmp(f,"soe")) return 0x151;
  if(!strcmp(f,"sekf")) return 0x140;
  if(!strcmp(f,"msoc")) return 0x146;
  if(!strcmp(f,"minsoc")||!strcmp(f,"v1")||!strcmp(f,"v2")||!strcmp(f,"dl")) return 0x142;
  if(!strcmp(f,"dd")||!strcmp(f,"dc")||!strcmp(f,"dr")) return 0x139;
  if(!strcmp(f,"cr")||!strcmp(f,"crl")||!strcmp(f,"cpq")) return 0x148;
  if(!strcmp(f,"odo")) return 0x212;
  return 0;
}
void handleChart(){
  String f=server.arg("f");
  const char *fs=f.c_str();
  uint16_t id=chartIdFor(fs);
  if(id==0){ server.send(400,"application/json","{\"v\":0}"); return; }
  int32_t mv=0;
  xSemaphoreTake(mtx, portMAX_DELAY);
  if(!strcmp(fs,"soc")) mv=(int32_t)snap.soc*10;
  else if(!strcmp(fs,"soh")) mv=(int32_t)snap.soh*10;
  else if(!strcmp(fs,"bdelta")) mv=(int32_t)snap.deltaSoc*10;
  else if(!strcmp(fs,"volt")) mv=(int32_t)snap.voltage*10;
  else if(!strcmp(fs,"imb")) mv=(int32_t)snap.imbalance/10;
  else if(!strcmp(fs,"rpm")) mv=(int32_t)snap.rpm*1000;
  else if(!strcmp(fs,"range")) mv=(int32_t)snap.rangeKm*1000;
  else if(!strcmp(fs,"coutv")) mv=(int32_t)snap.chargerOV;
  else if(!strcmp(fs,"couti")) mv=(int32_t)snap.chargerOI;
  else if(!strcmp(fs,"cinv")) mv=(int32_t)snap.chargerIV;
  else if(!strcmp(fs,"watts")){ uint64_t pp=(uint64_t)snap.chargerOV*(uint64_t)snap.chargerOI; mv=(int32_t)(pp/1000ULL); }
  else if(!strcmp(fs,"mbi")) mv=(int32_t)snap.motorI*100;
  else if(!strcmp(fs,"mt")) mv=(int32_t)snap.motorTemp*1000;
  else if(!strcmp(fs,"ctemp")) mv=(int32_t)snap.controllerTemp*1000;
  else if(!strcmp(fs,"thro")) mv=(int32_t)snap.throttleOut*1000;
  else if(!strcmp(fs,"thr1")) mv=(int32_t)snap.throttle1*1000;
  else if(!strcmp(fs,"thr3")) mv=(int32_t)snap.throttle3*1000;
  else if(!strcmp(fs,"hum")) mv=(int32_t)snap.humidity*10;
  else if(!strcmp(fs,"amb")) mv=(int32_t)snap.ambientTemp*10;
  else if(!strcmp(fs,"maxS")) mv=(int32_t)snap.maxCellSoh*10;
  else if(!strcmp(fs,"minS")) mv=(int32_t)snap.minCellSoh*10;
  else if(!strcmp(fs,"pdu")) mv=(int32_t)snap.pduVoltage;
  else if(!strcmp(fs,"fan1")) mv=(int32_t)snap.chargerFan1*1000;
  else if(!strcmp(fs,"fan2")) mv=(int32_t)snap.chargerFan2*1000;
  else if(!strcmp(fs,"bal")) mv=(int32_t)snap.balancing*1000;
  else if(!strcmp(fs,"ct0")) mv=(int32_t)snap.cellTemp[0]*10;
  else if(!strcmp(fs,"ct1")) mv=(int32_t)snap.cellTemp[1]*10;
  else if(!strcmp(fs,"ct2")) mv=(int32_t)snap.cellTemp[2]*10;
  else if(!strcmp(fs,"ct3")) mv=(int32_t)snap.cellTemp[3]*10;
  else if(!strcmp(fs,"ct4")) mv=(int32_t)snap.cellTemp[4]*10;
  else if(!strcmp(fs,"die")) mv=(int32_t)snap.afeDieTemp*10;
  else if(!strcmp(fs,"eft")) mv=(int32_t)snap.eftTemp*10;
  else if(!strcmp(fs,"eci")) mv=(int32_t)snap.eciTemp*10;
  else if(!strcmp(fs,"sr")) mv=(int32_t)snap.senseTemp*10;
  else if(!strcmp(fs,"ci")) mv=(int32_t)snap.packCurrent*10;
  else if(!strcmp(fs,"dd")) mv=(int32_t)snap.deratedDsgI*10;
  else if(!strcmp(fs,"dc")) mv=(int32_t)snap.deratedChgI*10;
  else if(!strcmp(fs,"dr")) mv=(int32_t)snap.deratedRegenI*10;
  else if(!strcmp(fs,"dl")) mv=(int32_t)snap.dsgLimTemp*10;
  else if(!strcmp(fs,"cr")) mv=(int32_t)snap.chgCellRule*10;
  else if(!strcmp(fs,"crl")) mv=(int32_t)snap.chgCmdRateLtd*10;
  else if(!strcmp(fs,"cpq")) mv=(int32_t)snap.chgPostQuant*10;
  else if(!strcmp(fs,"soe")) mv=(int32_t)snap.soe*10;
  else if(!strcmp(fs,"sekf")) mv=(int32_t)snap.socEkf*10;
  else if(!strcmp(fs,"msoc")) mv=(int32_t)snap.maxSoc*10;
  else if(!strcmp(fs,"minsoc")) mv=(int32_t)snap.minSocEst*10;
  else if(!strcmp(fs,"v1")) mv=(int32_t)snap.minV1Est/10;
  else if(!strcmp(fs,"v2")) mv=(int32_t)snap.minV2Est/10;
  else if(!strcmp(fs,"odo")) mv=(int32_t)snap.odometer;
  xSemaphoreGive(mtx);
  chartOn=true; chartId=id; chartUntil=millis()+50000UL;
  snprintf(chartBuf,sizeof(chartBuf),"{\"v\":%d}",(int)mv);
  server.send(200,"application/json",chartBuf);
}
void handleChartOff(){
  chartOn=false;
  server.send(200,"application/json","{\"off\":1}");
}

// ---------------------------------------------------------------------------
// Offline log: 1 Hz ring buffer in RAM, deleted once the phone confirms it
// ---------------------------------------------------------------------------

// Oldest sequence number still held on the device. Samples <= logAcked have been
// confirmed by the phone and are deleted; samples older than LOG_CAP are overwritten.
static inline uint32_t logOldestSeq(){
  uint32_t lo = (logHead > LOG_CAP) ? logHead - LOG_CAP + 1 : 1;
  if (logAcked + 1 > lo) lo = logAcked + 1;
  return lo;
}

// Must match the checksum computed in the browser.
static inline uint32_t logSampleSum(uint32_t seq, const LogSample &e){
  return seq + e.t + (uint32_t)e.voltage + (uint32_t)e.soc + (uint32_t)(uint16_t)e.motorTemp
       + (uint32_t)e.range + (uint32_t)e.mode + (uint32_t)(uint16_t)e.motorI
       + (uint32_t)e.rpm + e.odo;
}

void logSampleTick(){
  unsigned long now = millis();
  if (now - lastLogSample < LOG_INTERVAL_MS) return;
  lastLogSample = now;

  const uint16_t v   = snap.voltage;
  const uint16_t sc  = snap.soc;
  const int16_t  mt  = snap.motorTemp;
  const int      rg  = snap.rangeKm;
  const int      dm  = snap.driveMode;
  const int16_t  mi  = snap.motorI;
  const int      rp  = snap.rpm;
  const uint32_t od  = snap.odometer;

  // NOTE: samples are logged even when voltage/other values are 0, so the
  // charts also show the periods when the vehicle / CAN bus was not alive.
  uint32_t seq = logHead + 1;
  LogSample &e = logBuf[seq % LOG_CAP];
  e.t         = (uint32_t)(now / 1000UL);
  e.voltage   = v;
  e.soc       = sc;
  e.motorTemp = mt;
  e.range     = (uint8_t)(rg < 0 ? 0 : (rg > 255 ? 255 : rg));
  e.mode      = (uint8_t)(dm < 0 ? 0 : (dm > 255 ? 255 : dm));
  e.motorI    = mi;
  e.rpm       = (uint16_t)(rp < 0 ? 0 : (rp > 65535 ? 65535 : rp));
  e.odo       = od;
  logHead     = seq;
}

// GET /log?since=N  -> samples with seq > N (max LOG_MAX_PER_REQ), with count + checksum
void handleLog(){
  uint32_t since = server.hasArg("since") ? (uint32_t)strtoul(server.arg("since").c_str(), NULL, 10) : 0;
  const uint32_t head   = logHead;
  const uint32_t oldest = logOldestSeq();

  uint32_t first = since + 1, dropped = 0, n = 0, last = 0, cs = 0;
  if (first < oldest) { dropped = oldest - first; first = oldest; }   // already delivered/deleted, or overwritten

  if (first <= head) {
    n = head - first + 1;
    if (n > LOG_MAX_PER_REQ) n = LOG_MAX_PER_REQ;
    last = first + n - 1;
    for (uint32_t s = first; s <= last; s++) cs += logSampleSum(s, logBuf[s % LOG_CAP]);
  } else {
    first = 0;                            // nothing new
  }

  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char out[1024];
  size_t p = 0;
  p += snprintf(out + p, sizeof(out) - p,
    "{\"boot\":%lu,\"now\":%lu,\"oldest\":%lu,\"head\":%lu,\"first\":%lu,\"last\":%lu,"
    "\"n\":%lu,\"max\":%d,\"cs\":%lu,\"dropped\":%lu,\"s\":[",
    (unsigned long)logBootId, (unsigned long)(millis() / 1000UL),
    (unsigned long)oldest, (unsigned long)head,
    (unsigned long)first, (unsigned long)last, (unsigned long)n,
    (int)LOG_MAX_PER_REQ, (unsigned long)cs, (unsigned long)dropped);

  for (uint32_t i = 0; i < n; i++) {
    if (p > sizeof(out) - 128) { out[p] = 0; server.sendContent(out); p = 0; }
    const uint32_t s = first + i;
    const LogSample &e = logBuf[s % LOG_CAP];
    p += snprintf(out + p, sizeof(out) - p, "%s[%lu,%lu,%u,%u,%d,%u,%u,%d,%u,%lu]",
                  i ? "," : "", (unsigned long)s, (unsigned long)e.t,
                  (unsigned)e.voltage, (unsigned)e.soc, (int)e.motorTemp,
                  (unsigned)e.range, (unsigned)e.mode, (int)e.motorI,
                  (unsigned)e.rpm, (unsigned long)e.odo);
  }
  p += snprintf(out + p, sizeof(out) - p, "]}");
  out[p] = 0;
  server.sendContent(out);
  server.sendContent("");                 // terminate chunked response

  if (n > 0 && last > logSent) logSent = last;
}

// GET /ack?seq=N&boot=B -> browser confirms it received & PERSISTED everything up to N.
char ackBuf[96];
void handleAck(){
  uint32_t seq  = server.hasArg("seq")  ? (uint32_t)strtoul(server.arg("seq").c_str(),  NULL, 10) : 0;
  uint32_t boot = server.hasArg("boot") ? (uint32_t)strtoul(server.arg("boot").c_str(), NULL, 10) : 0;
  bool ok = (boot == logBootId) && (seq <= logSent);   // only accept acks for data actually sent
  if (ok && seq > logAcked) logAcked = seq;            // advancing this deletes everything <= seq
  snprintf(ackBuf, sizeof(ackBuf), "{\"ok\":%d,\"acked\":%lu,\"head\":%lu}",
           ok ? 1 : 0, (unsigned long)logAcked, (unsigned long)logHead);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", ackBuf);
}

void setupCAN(){
  pinMode(CAN_RX, INPUT_PULLUP);
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX,(gpio_num_t)CAN_RX,TWAI_MODE_NORMAL);
  g.rx_queue_len=128; g.tx_queue_len=0;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  ESP_ERROR_CHECK(twai_driver_install(&g,&t,&f));
  ESP_ERROR_CHECK(twai_start());
}

void IRAM_ATTR applyFrame(Telemetry &t, const twai_message_t &m){
  if(m.extd||m.rtr) return;
  if(chartOn){
    if((long)(millis()-chartUntil)>=0) chartOn=false;
    else if(m.identifier!=chartId) return;
  }
  const uint8_t *d=m.data;
  switch(m.identifier){
    case 0x131: if(m.data_length_code<6)return; t.soc=d[4]+(d[5]<<8); t.balancing=((d[2]>>4)|((d[3]&0x0F)<<4))&1; t.chargerConnected=((d[2]>>6)&1); break;
    case 0x14D: if(m.data_length_code<8)return; t.soh=d[4]+(d[5]<<8); t.maxCellSoh=d[4]+(d[5]<<8); t.minCellSoh=d[6]+(d[7]<<8); break;
    case 0x13D: if(m.data_length_code<6)return; t.deltaSoc=d[4]+(d[5]<<8); break;
    case 0x141: if(m.data_length_code<8)return; t.voltage=d[0]+(d[1]<<8); t.imbalance=d[2]+(d[3]<<8); break;
    case 0x200: if(m.data_length_code<8)return; t.rangeKm=d[6]; t.displayFan=((d[7]>>4)&1); break;
    case 0x510: if(m.data_length_code<4)return; { int16_t raw=(int16_t)(d[2]+(d[3]<<8)); t.rpm=raw*5/6; } break;
    case 0x101: if(m.data_length_code<5)return; t.driveMode=d[1]; t.smartEco=((d[3]>>6)&1); t.forcedEco=((d[4]>>2)&1); break;
    case 0x102:
      if(m.data_length_code<8)return;
      t.pduVoltage=d[6]+(d[7]<<8);
      t.sideStand=(d[4]&1);
      t.frontBrake=((d[0]>>0)&1); t.rearBrake=((d[0]>>1)&1); t.highBeam=((d[0]>>2)&1);
      t.startSwitch=((d[0]>>4)&1); t.killSwitch=((d[0]>>5)&1); t.storageSwitch=((d[0]>>6)&1);
      t.horn=((d[1]>>1)&1); t.indicatorLeftSide=((d[2]>>1)&1); t.indicatorRightSide=((d[2]>>0)&1); t.indicatorCenterSide=((d[2]>>2)&1); t.keyOn=((d[4]>>3)&1);
      break;
    case 0x103:
      if(m.data_length_code<2)return;
      t.seatLock=((d[1]>>1)&1); t.chargerLock2=((d[1]>>2)&1); t.chargerLock1=((d[1]>>3)&1);
      break;
    case 0x208:
      if(m.data_length_code<4)return;
      t.chargerFan1=d[0]+(d[1]<<8); t.chargerFan2=d[2]+(d[3]<<8);
      break;
    case 0x132: if(m.data_length_code<8)return; t.cellV[0]=d[0]+(d[1]<<8); t.cellV[1]=d[2]+(d[3]<<8); t.cellV[2]=d[4]+(d[5]<<8); t.cellV[3]=d[6]+(d[7]<<8); break;
    case 0x133: if(m.data_length_code<8)return; t.cellV[4]=d[0]+(d[1]<<8); t.cellV[5]=d[2]+(d[3]<<8); t.cellV[6]=d[4]+(d[5]<<8); t.cellV[7]=d[6]+(d[7]<<8); break;
    case 0x134: if(m.data_length_code<8)return; t.cellV[8]=d[0]+(d[1]<<8); t.cellV[9]=d[2]+(d[3]<<8); t.cellV[10]=d[4]+(d[5]<<8); t.cellV[11]=d[6]+(d[7]<<8); break;
    case 0x135: if(m.data_length_code<4)return; t.cellV[12]=d[0]+(d[1]<<8); t.cellV[13]=d[2]+(d[3]<<8); if(m.data_length_code>=8){ t.afeDieTemp=(int16_t)(d[4]+(d[5]<<8)); t.packCurrent=(int16_t)(d[6]+(d[7]<<8)); } break;
    case 0x14e: if(m.data_length_code<8)return; t.cellSoh[0]=d[0]; t.cellSoh[1]=d[1]; t.cellSoh[2]=d[2]; t.cellSoh[3]=d[3]; t.cellSoh[4]=d[4]; t.cellSoh[5]=d[5]; t.cellSoh[6]=d[6]; t.cellSoh[7]=d[7]; break;
    case 0x14f: if(m.data_length_code<6)return; t.cellSoh[8]=d[0]; t.cellSoh[9]=d[1]; t.cellSoh[10]=d[2]; t.cellSoh[11]=d[3]; t.cellSoh[12]=d[4]; t.cellSoh[13]=d[5]; break;
    case 0x136: if(m.data_length_code<8)return; for(int i=0;i<4;i++) t.cellTemp[i]=(int16_t)(d[i*2]+(d[i*2+1]<<8)); break;
    case 0x137: if(m.data_length_code<8)return; t.cellTemp[4]=(int16_t)(d[0]+(d[1]<<8)); t.eftTemp=(int16_t)(d[2]+(d[3]<<8)); t.eciTemp=(int16_t)(d[4]+(d[5]<<8)); t.senseTemp=(int16_t)(d[6]+(d[7]<<8)); break;
    case 0x139: if(m.data_length_code<6)return; t.deratedDsgI=d[0]+(d[1]<<8); t.deratedChgI=d[2]+(d[3]<<8); t.deratedRegenI=d[4]+(d[5]<<8); break;
    case 0x142: if(m.data_length_code<8)return; t.minV1Est=(int16_t)(d[0]+(d[1]<<8)); t.minV2Est=(int16_t)(d[2]+(d[3]<<8)); t.minSocEst=d[4]+(d[5]<<8); t.dsgLimTemp=d[6]+(d[7]<<8); break;
    case 0x143: if(m.data_length_code<8)return; for(int i=0;i<4;i++) t.cellSoc[i]=d[i*2]+(d[i*2+1]<<8); break;
    case 0x144: if(m.data_length_code<8)return; for(int i=0;i<4;i++) t.cellSoc[4+i]=d[i*2]+(d[i*2+1]<<8); break;
    case 0x145: if(m.data_length_code<8)return; for(int i=0;i<4;i++) t.cellSoc[8+i]=d[i*2]+(d[i*2+1]<<8); break;
    case 0x146: if(m.data_length_code<6)return; t.cellSoc[12]=d[0]+(d[1]<<8); t.cellSoc[13]=d[2]+(d[3]<<8); t.maxSoc=d[4]+(d[5]<<8); break;
    case 0x148: if(m.data_length_code<6)return; t.chgCellRule=d[0]+(d[1]<<8); t.chgCmdRateLtd=d[2]+(d[3]<<8); t.chgPostQuant=(int16_t)(d[4]+(d[5]<<8)); break;
    case 0x151: if(m.data_length_code<6)return; t.soe=d[4]+(d[5]<<8); break;
    case 0x212: if(m.data_length_code<4)return; t.odometer=d[0]|(d[1]<<8)|(d[2]<<16)|((uint32_t)d[3]<<24); break;
    case 0x162: if(m.data_length_code<7)return; t.bmsHwVer=d[1]; t.bmsFw[0]=d[4]; t.bmsFw[1]=d[5]; t.bmsFw[2]=d[6]; break;
    case 0x107: if(m.data_length_code<7)return; t.bcmFw[0]=d[4]; t.bcmFw[1]=d[5]; t.bcmFw[2]=d[6]; break;
    case 0x216: if(m.data_length_code<7)return; t.gtwFw[0]=d[4]; t.gtwFw[1]=d[5]; t.gtwFw[2]=d[6]; break;
    case 0x70B: if(m.data_length_code<4)return; t.motorFw[0]=d[1]; t.motorFw[1]=d[2]; t.motorFw[2]=d[3]; break;
    case 0x163: if(m.data_length_code<8)return; for(int i=0;i<6;i++) t.binId[i]=(char)d[2+i]; break;
    case 0x164: if(m.data_length_code<8)return; for(int i=0;i<8;i++) t.binId[6+i]=(char)d[i]; t.binId[14]=0; break;
    case 0x201:
      if(m.data_length_code<8)return;
      t.chargerOV=d[0]|(d[1]<<8)|(d[2]<<16)|((uint32_t)d[3]<<24);
      t.chargerOI=d[4]|(d[5]<<8)|(d[6]<<16)|((uint32_t)d[7]<<24);
      break;
    case 0x203:
      if(m.data_length_code<4)return;
      t.chargerIV=d[0]|(d[1]<<8)|(d[2]<<16)|((uint32_t)d[3]<<24);
      break;
    case 0x18c:
      if(m.data_length_code<6)return;
      t.motorI=((d[2]>>5)|((d[3]&31)<<3))+((((d[3]>>5)|((d[4]&31)<<3))&1)*256)-(((d[3]>>5)&1)*512);
      t.motorTemp=(((d[4]&63)<<2)|(d[3]>>6))-40;
      t.controllerTemp=(((d[5]&63)<<2)|(d[4]>>6))-40;
      break;
    case 0x38c:
      if(m.data_length_code<8)return;
      t.throttleOut=((d[3]>>2)|((d[4]&3)<<6))-(((d[4]>>1)&1)*256);
      t.throttle3=((d[5]>>4)|((d[6]&15)<<4))-(((d[6]>>3)&1)*256);
      t.throttle1=((d[6]>>4)|((d[7]&15)<<4))-(((d[7]>>3)&1)*256);
      break;
    case 0x140:
      if(m.data_length_code<4)return;
      t.humidity=d[0]+(d[1]<<8);
      { int32_t r=d[2]+(d[3]<<8); if(d[3]>>7) r-=65536; t.ambientTemp=(int16_t)r; }
      if(m.data_length_code>=6) t.socEkf=d[4]+(d[5]<<8);
      break;
  }
}

void canTask(void *){
  Telemetry local;
  xSemaphoreTake(mtx, portMAX_DELAY);
  local = snap;
  xSemaphoreGive(mtx);
  uint32_t backlog = 0;
  for(;;){
    twai_message_t msg;
    if(twai_receive(&msg,0)==ESP_OK){
      applyFrame(local, msg);
      backlog++;
    } else {
      if(backlog>0){
        xSemaphoreTake(mtx, portMAX_DELAY);
        snap = local;
        xSemaphoreGive(mtx);
        backlog=0;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void setup(){
  mtx = xSemaphoreCreateMutex();          // created first so nothing can take a null handle
  logBootId = esp_random();
  WiFi.persistent(false); WiFi.disconnect(true,true); delay(200);
  WiFi.mode(WIFI_AP); delay(200);
  WiFi.softAP(AP_SSID,AP_PASS,6,0,4); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.on("/",handleRoot); server.on("/data",handleData); server.on("/cells",handleCells); server.on("/charger",handleCharger); server.on("/extra",handleExtra); server.on("/all",handleAll); server.on("/chart",handleChart); server.on("/chartoff",handleChartOff); server.on("/log",handleLog); server.on("/ack",handleAck); server.begin();
  setupCAN();
  xTaskCreatePinnedToCore(canTask,"can",4096,NULL,2,NULL,0);
}

void loop(){
  server.handleClient();
  logSampleTick();
  vTaskDelay(pdMS_TO_TICKS(2));
}
