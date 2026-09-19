#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>

#define CAN_TX 10
#define CAN_RX 20
const char* AP_SSID = "ATHER-OBD";
const char* AP_PASS = "12345678";

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
};

Telemetry snap;
SemaphoreHandle_t mtx;
WebServer server(80);
unsigned long lastDiag = 0;
uint32_t maxQueueBacklog = 0;

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
<title>Ather OBD Scanner v3</title>
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
.vbar{display:inline-block;height:8px;border-radius:4px;vertical-align:middle;margin-left:4px;transition:width .3s;}
.panel{display:none;}
.panel.active{display:block;}
</style>
<script>
let curTab='dash',busy=false;
function showTab(t){
  curTab=t;
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(b=>b.classList.remove('active'));
  document.getElementById(t).classList.add('active');
  document.getElementById('tb_'+t).classList.add('active');
  if(t==='dash') update();
  if(t==='cells') updateCells();
  if(t==='charger') updateCharger();
}
function sw(id,val,onTxt,offTxt){let e=document.getElementById(id);if(!e)return;e.innerText=val?onTxt:offTxt;e.className=val?'on':'off';}
async function update(){
 if(curTab!=='dash'||busy)return;
 busy=true;
 try{
  let r=await fetch('/data');let j=await r.json();
  document.getElementById('soc').innerText=j.soc.toFixed(2)+' %';
  document.getElementById('soh').innerText=j.soh.toFixed(2)+' %';
  document.getElementById('bdelta').innerText=j.bdelta.toFixed(2)+' %';
  document.getElementById('volt').innerText=j.volt.toFixed(2)+' V';
  document.getElementById('imb').innerText=j.imb.toFixed(4)+' V';
  document.getElementById('rpm').innerText=j.rpm;
  document.getElementById('mode').innerText=j.mode;
  document.getElementById('range').innerText=j.range.toFixed(0)+' km';
  sw('fbrake',j.fbrake,'Pressed','Not Pressed');
  sw('rbrake',j.rbrake,'Pressed','Not Pressed');
  sw('hbeam',j.hbeam,'Pressed','Not Pressed');
  sw('start',j.start,'Pressed','Not Pressed');
  sw('kill',j.kill,'Pressed','Not Pressed');
  sw('storage',j.storage,'Pressed','Not Pressed');
  sw('horn',j.horn,'Pressed','Not Pressed');
  sw('indl',j.indl,'Pressed','Not Pressed');
  sw('indr',j.indr,'Pressed','Not Pressed');
  sw('indc',j.indc,'Pressed','Not Pressed');
  sw('key',j.key,'Enabled','Disabled');
  sw('stand',j.stand,'DOWN','UP');
 }catch(e){}
 busy=false;
}
async function updateCells(){
 if(curTab!=='cells'||busy)return;
 busy=true;
 try{
  let r=await fetch('/cells');let j=await r.json();
  let pos=j.v.filter(x=>x>0);
  let mn=pos.length?Math.min(...pos):0;
  let mx=pos.length?Math.max(...j.v):0;
  let rng=mx-mn||1;
  for(let i=0;i<14;i++){
    document.getElementById('cv'+i).innerText=j.v[i].toFixed(4)+' V';
    document.getElementById('sh'+i).innerText=j.h[i].toFixed(0)+' %';
    let bw=mx>0?((j.v[i]-mn)/rng*60+4):0;
    let bar=document.getElementById('vb'+i);
    bar.style.width=bw+'px';
    let dev=Math.abs(j.v[i]-((mn+mx)/2));
    bar.style.background=dev<0.01?'#5a7a6a':dev<0.03?'#e6a817':'#d94f4f';
    let shb=document.getElementById('hb'+i);
    if(shb){shb.style.background=j.h[i]>90?'#5a7a6a':j.h[i]>70?'#e6a817':'#d94f4f';shb.style.width=(j.h[i])+'px';}
  }
 }catch(e){}
 busy=false;
}
async function updateCharger(){
 if(curTab!=='charger'||busy)return;
 busy=true;
 try{
  let r=await fetch('/charger');let j=await r.json();
  document.getElementById('coutv').innerText=j.coutv.toFixed(2)+' V';
  document.getElementById('couti').innerText=j.couti.toFixed(2)+' A';
  document.getElementById('cinv').innerText=j.cinv.toFixed(2)+' V';
  document.getElementById('mbi').innerText=j.mbi.toFixed(2)+' A';
  document.getElementById('mt').innerText=j.mt.toFixed(1)+' \u00B0C';
  document.getElementById('bal').innerText=j.bal;
  let w=j.watts;
  document.getElementById('cwatts').innerText=w>=1000?(w/1000).toFixed(2)+' kW':w.toFixed(1)+' W';
 }catch(e){}
 busy=false;
}
setInterval(update,1000);
setInterval(updateCells,1500);
setInterval(updateCharger,1500);
window.onload=function(){showTab('dash');};
</script></head><body>
<header><h1>&#9889; Ather OBD Scanner v3</h1><p style="margin:6px 0 0;color:var(--muted);font-size:13px;">Developed by <b style="color:var(--text);">sam0_0</b> &bull; Ather OBD Scanner</p><p style="margin:10px 0 0;"><a href="https://www.buymeacoffee.com/sam0_0" target="_blank" style="display:inline-block;background:#ffdd00;color:#1a1a1a;padding:7px 16px;border-radius:999px;font-size:13px;font-weight:700;text-decoration:none;box-shadow:0 2px 6px rgba(0,0,0,0.12);">&#9749; Support this project &mdash; Buy Me a Coffee</a></p></header>
<div class="tabs">
<button class="tab active" id="tb_dash" onclick="showTab('dash')">Dashboard</button>
<button class="tab" id="tb_cells" onclick="showTab('cells')">Cell Info</button>
<button class="tab" id="tb_charger" onclick="showTab('charger')">Charger</button>
</div>

<div id="dash" class="panel active">
<div class="card"><h2>Battery &amp; Drive</h2>
<table>
<tr><td>SoC</td><td class='v' id='soc'>--</td></tr>
<tr><td>SoH</td><td class='v' id='soh'>--</td></tr>
<tr><td>Delta SoC</td><td class='v' id='bdelta'>--</td></tr>
<tr><td>Voltage</td><td class='v' id='volt'>--</td></tr>
<tr><td>Voltage Imbalance</td><td class='v' id='imb'>--</td></tr>
<tr><td>Motor RPM</td><td class='v' id='rpm'>--</td></tr>
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
</table></div>
</div>

<div id="cells" class="panel">
<div class="card ctable"><h2>Cell Voltages (14S)</h2>
<table><tr><th>Cell</th><th>Voltage</th><th style="width:80px">Bar</th></tr>
<tbody id="cvtable"></tbody></table></div>
<div class="card ctable"><h2>Cell State of Health</h2>
<table><tr><th>Cell</th><th>SoH</th><th style="width:80px">Bar</th></tr>
<tbody id="cshtable"></tbody></table></div>
</div>

<div id="charger" class="panel">
<div class="card"><h2>Charger</h2>
<table>
<tr><td>Output Voltage</td><td class='v' id='coutv'>--</td></tr>
<tr><td>Output Current</td><td class='v' id='couti'>--</td></tr>
<tr><td>Input Voltage</td><td class='v' id='cinv'>--</td></tr>
<tr><td>Power [calculated]</td><td class='v' id='cwatts'>--</td></tr>
<tr><td>Balancing State</td><td class='v' id='bal'>--</td></tr>
</table></div>
<div class="card"><h2>Motor</h2>
<table>
<tr><td>Battery Current</td><td class='v' id='mbi'>--</td></tr>
<tr><td>Temperature</td><td class='v' id='mt'>--</td></tr>
</table></div>
</div>

<script>
(function(){
  let cv='',sh='';
  for(let i=0;i<14;i++){
    cv+='<tr><td>C'+(i+1)+'</td><td id="cv'+i+'">--</td><td><div class="vbar" id="vb'+i+'" style="width:4px"></div></td></tr>';
    sh+='<tr><td>C'+(i+1)+'</td><td id="sh'+i+'">--</td><td><div class="vbar" id="hb'+i+'" style="width:0px;height:8px;border-radius:4px;"></div></td></tr>';
  }
  document.getElementById('cvtable').innerHTML=cv;
  document.getElementById('cshtable').innerHTML=sh;
})();
</script>
</body></html>
)HTML";

void handleRoot(){ server.send_P(200, "text/html", htmlPage); }
char jsonBuf[512];
void handleData(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  int n=snprintf(jsonBuf,sizeof(jsonBuf),
    "{\"soc\":%d.%02d,\"soh\":%d.%02d,\"bdelta\":%d.%02d,\"volt\":%d.%02d,\"imb\":%d.%04d,\"rpm\":%d,\"mode\":\"%s\",\"range\":%d,\"fbrake\":%s,\"rbrake\":%s,\"hbeam\":%s,\"start\":%s,\"kill\":%s,\"storage\":%s,\"horn\":%s,\"indl\":%s,\"indr\":%s,\"indc\":%s,\"key\":%s,\"stand\":%s}",
    t.soc/100,abs(t.soc%100), t.soh/100,abs(t.soh%100), t.deltaSoc/100,abs(t.deltaSoc%100),
    t.voltage/100,abs(t.voltage%100), t.imbalance/10000,abs(t.imbalance%10000),
    t.rpm,driveModeName(t.driveMode),t.rangeKm,
    t.frontBrake?"true":"false",t.rearBrake?"true":"false",t.highBeam?"true":"false",t.startSwitch?"true":"false",
    t.killSwitch?"true":"false",t.storageSwitch?"true":"false",t.horn?"true":"false",
    t.indicatorLeftSide?"true":"false",t.indicatorRightSide?"true":"false",t.indicatorCenterSide?"true":"false",t.keyOn?"true":"false",t.sideStand?"true":"false");
  if(n<0||n>=(int)sizeof(jsonBuf)){server.send(500,"application/json","{}");return;}
  server.send(200,"application/json",jsonBuf);
}

char cellBuf[512];
void handleCells(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  char *p=cellBuf; int rem=sizeof(cellBuf);
  int n=snprintf(p,rem,"{\"v\":["); if(n<0)return; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d.%04d%s",t.cellV[i]/10000,abs(t.cellV[i]%10000),i<13?",":""); if(n<0)return; p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"h\":["); if(n<0)return; p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%d%s",t.cellSoh[i],i<13?",":""); if(n<0)return; p+=n; rem-=n; }
  snprintf(p,rem,"]}");
  server.send(200,"application/json",cellBuf);
}

char chargerBuf[256];
void handleCharger(){
  xSemaphoreTake(mtx, portMAX_DELAY);
  Telemetry t = snap;
  xSemaphoreGive(mtx);
  const int32_t motorI=t.motorI;
  const int32_t motorMag=motorI<0?-motorI:motorI;
  const char *motorSign=motorI<0?"-":"";
  const uint64_t powerProduct=(uint64_t)t.chargerOV*(uint64_t)t.chargerOI;
  const uint32_t wattsWhole=(uint32_t)(powerProduct/1000000ULL);
  const unsigned wattsFrac=(unsigned)((powerProduct%1000000ULL)/10000ULL);
  snprintf(chargerBuf,sizeof(chargerBuf),
    "{\"coutv\":%d.%03d,\"couti\":%d.%03d,\"cinv\":%d.%03d,\"mbi\":%s%d.%01d,\"mt\":%d,\"bal\":%d,\"watts\":%u.%02u}",
    (int)(t.chargerOV/1000),abs((int)(t.chargerOV%1000)),
    (int)(t.chargerOI/1000),abs((int)(t.chargerOI%1000)),
    (int)(t.chargerIV/1000),abs((int)(t.chargerIV%1000)),
    motorSign,(int)(motorMag/10),abs((int)(motorMag%10)),
    (int)t.motorTemp, t.balancing, wattsWhole, wattsFrac);
  server.send(200,"application/json",chargerBuf);
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
  const uint8_t *d=m.data;
  switch(m.identifier){
    case 0x131: if(m.data_length_code<6)return; t.soc=d[4]+(d[5]<<8); t.balancing=((d[2]>>4)|((d[3]&0x0F)<<4))&1; break;
    case 0x14D: if(m.data_length_code<6)return; t.soh=d[4]+(d[5]<<8); break;
    case 0x13D: if(m.data_length_code<6)return; t.deltaSoc=d[4]+(d[5]<<8); break;
    case 0x141: if(m.data_length_code<8)return; t.voltage=d[0]+(d[1]<<8); t.imbalance=d[2]+(d[3]<<8); break;
    case 0x200: if(m.data_length_code<7)return; t.rangeKm=d[6]; break;
    case 0x510: if(m.data_length_code<4)return; { int16_t raw=(int16_t)(d[2]+(d[3]<<8)); t.rpm=raw*5/6; } break;
    case 0x101: if(m.data_length_code<2)return; t.driveMode=d[1]; break;
    case 0x102:
      if(m.data_length_code<5)return;
      t.sideStand=(d[4]&1);
      t.frontBrake=((d[0]>>0)&1); t.rearBrake=((d[0]>>1)&1); t.highBeam=((d[0]>>2)&1);
      t.startSwitch=((d[0]>>4)&1); t.killSwitch=((d[0]>>5)&1); t.storageSwitch=((d[0]>>6)&1);
      t.horn=((d[1]>>1)&1); t.indicatorLeftSide=((d[2]>>0)&1); t.indicatorRightSide=((d[2]>>1)&1); t.indicatorCenterSide=((d[2]>>2)&1); t.keyOn=((d[4]>>3)&1);
      break;
    case 0x132: if(m.data_length_code<8)return; t.cellV[0]=d[0]+(d[1]<<8); t.cellV[1]=d[2]+(d[3]<<8); t.cellV[2]=d[4]+(d[5]<<8); t.cellV[3]=d[6]+(d[7]<<8); break;
    case 0x133: if(m.data_length_code<8)return; t.cellV[4]=d[0]+(d[1]<<8); t.cellV[5]=d[2]+(d[3]<<8); t.cellV[6]=d[4]+(d[5]<<8); t.cellV[7]=d[6]+(d[7]<<8); break;
    case 0x134: if(m.data_length_code<8)return; t.cellV[8]=d[0]+(d[1]<<8); t.cellV[9]=d[2]+(d[3]<<8); t.cellV[10]=d[4]+(d[5]<<8); t.cellV[11]=d[6]+(d[7]<<8); break;
    case 0x135: if(m.data_length_code<4)return; t.cellV[12]=d[0]+(d[1]<<8); t.cellV[13]=d[2]+(d[3]<<8); break;
    case 0x14e: if(m.data_length_code<8)return; t.cellSoh[0]=d[0]; t.cellSoh[1]=d[1]; t.cellSoh[2]=d[2]; t.cellSoh[3]=d[3]; t.cellSoh[4]=d[4]; t.cellSoh[5]=d[5]; t.cellSoh[6]=d[6]; t.cellSoh[7]=d[7]; break;
    case 0x14f: if(m.data_length_code<6)return; t.cellSoh[8]=d[0]; t.cellSoh[9]=d[1]; t.cellSoh[10]=d[2]; t.cellSoh[11]=d[3]; t.cellSoh[12]=d[4]; t.cellSoh[13]=d[5]; break;
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
      if(m.data_length_code<5)return;
      t.motorI=((d[2]>>5)|((d[3]&31)<<3))+((((d[3]>>5)|((d[4]&31)<<3))&1)*256)-(((d[3]>>5)&1)*512);
      t.motorTemp=(((d[4]&63)<<2)|(d[3]>>6))-40;
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
        if(backlog>maxQueueBacklog) maxQueueBacklog=backlog;
        backlog=0;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void setup(){
  Serial.begin(115200);
  WiFi.persistent(false); WiFi.disconnect(true,true); delay(200);
  WiFi.mode(WIFI_AP); delay(200);
  WiFi.softAP(AP_SSID,AP_PASS,6,0,4); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.on("/",handleRoot); server.on("/data",handleData); server.on("/cells",handleCells); server.on("/charger",handleCharger); server.begin();
  setupCAN();
  mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(canTask,"can",4096,NULL,2,NULL,0);
}

void loop(){
  server.handleClient();
  if(millis()-lastDiag>=5000){
    lastDiag=millis();
    twai_status_info_t st;
    if(twai_get_status_info(&st)==ESP_OK){
      Serial.printf("[DIAG] rx_missed:%lu rx_overrun:%lu arb_lost:%lu max_backlog:%lu heap:%lu\n",
        (unsigned long)st.rx_missed_count,(unsigned long)st.rx_overrun_count,
        (unsigned long)st.arb_lost_count,(unsigned long)maxQueueBacklog,
        (unsigned long)ESP.getFreeHeap());
    }
    maxQueueBacklog=0;
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}
