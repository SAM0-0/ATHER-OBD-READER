#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>

#define CAN_TX 10
#define CAN_RX 20
const char* AP_SSID = "ATHER-OBD";
const char* AP_PASS = "12345678";

volatile float soc = 0;
volatile float soh = 0;
volatile float deltaSoc = 0;
volatile float voltage = 0;
volatile float imbalance = 0;
volatile int   balancing = 0;
volatile int   rpm = 0;
volatile int   driveMode = 0;
volatile bool  sideStand = false;
volatile bool  frontBrake = false, rearBrake = false, highBeam = false, startSwitch = false;
volatile bool  killSwitch = false, storageSwitch = false, horn = false;
volatile bool  indicatorLeftSide = false, indicatorRightSide = false, indicatorCenterSide = false, keyOn = false;

float bootSoc = -1;
volatile int rangeKm = 0;
unsigned long lastPrint = 0;
uint32_t framesTotal = 0;
WebServer server(80);

volatile float cellVoltage[14] = {0};
volatile float cellSoh[14] = {0};

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
<title>Ather OBD Scanner v2</title>
<style>
:root{--bg:#eceeef;--card:#fff;--text:#2b2e34;--muted:#6b7280;--border:#e2e6ea;--accent:#5a7a6a;}
*{box-sizing:border-box}
body{font-family:Inter,Arial,sans-serif;background:var(--bg);color:var(--text);margin:0;}
header{background:var(--card);border-bottom:1px solid var(--border);padding:14px 10px;text-align:center;}
header h1{margin:0;font-size:22px;}
.tabs{display:flex;justify-content:center;background:var(--card);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:10;}
.tab{flex:1;max-width:200px;padding:12px;border:none;background:var(--card);color:var(--muted);font-size:14px;font-weight:600;cursor:pointer;border-bottom:3px solid transparent;transition:all .2s;}
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
.sec-title{font-size:15px;font-weight:600;color:var(--text);margin:20px 0 8px;text-align:center;}
</style>
<script>
let curTab='dash';
function showTab(t){
  curTab=t;
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(b=>b.classList.remove('active'));
  document.getElementById(t).classList.add('active');
  document.getElementById('tb_'+t).classList.add('active');
}
function sw(id,val,onTxt,offTxt){let e=document.getElementById(id);if(!e)return;e.innerText=val?onTxt:offTxt;e.className=val?'on':'off';}
async function update(){
 if(curTab!=='dash')return;
 try{
  let r=await fetch('/data');let j=await r.json();
  document.getElementById('soc').innerText=j.soc.toFixed(2)+' %';
  document.getElementById('soh').innerText=j.soh.toFixed(2)+' %';
  document.getElementById('bdelta').innerText=j.bdelta.toFixed(2)+' %';
  document.getElementById('volt').innerText=j.volt.toFixed(2)+' V';
  document.getElementById('imb').innerText=j.imb.toFixed(4)+' V';
  document.getElementById('bal').innerText=j.bal;
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
}
async function updateCells(){
 if(curTab!=='cells')return;
 try{
  let r=await fetch('/cells');let j=await r.json();
  let mn=Math.min(...j.v.filter(x=>x>0));
  let mx=Math.max(...j.v);
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
}
setInterval(update,1000);
setInterval(updateCells,1500);
window.onload=function(){showTab('dash');update();};
</script></head><body>
<header><h1>&#9889; Ather OBD Scanner v2</h1><p style="margin:6px 0 0;color:var(--muted);font-size:13px;">Developed by <b style="color:var(--text);">sam0_0</b> &bull; Ather OBD Scanner</p><p style="margin:10px 0 0;"><a href="https://www.buymeacoffee.com/sam0_0" target="_blank" style="display:inline-block;background:#ffdd00;color:#1a1a1a;padding:7px 16px;border-radius:999px;font-size:13px;font-weight:700;text-decoration:none;box-shadow:0 2px 6px rgba(0,0,0,0.12);">&#9749; Support this project &mdash; Buy Me a Coffee</a></p></header>
<div class="tabs">
<button class="tab active" id="tb_dash" onclick="showTab('dash')">Dashboard</button>
<button class="tab" id="tb_cells" onclick="showTab('cells')">Cell Info</button>
</div>

<div id="dash" class="panel active">
<div class="card"><h2>Battery &amp; Drive</h2>
<table>
<tr><td>SoC</td><td class='v' id='soc'>--</td></tr>
<tr><td>SoH</td><td class='v' id='soh'>--</td></tr>
<tr><td>Delta SoC</td><td class='v' id='bdelta'>--</td></tr>
<tr><td>Voltage</td><td class='v' id='volt'>--</td></tr>
<tr><td>Voltage Imbalance</td><td class='v' id='imb'>--</td></tr>
<tr><td>Balancing State</td><td class='v' id='bal'>--</td></tr>
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

void handleRoot(){ server.send(200,"text/html",htmlPage); }
char jsonBuf[512];
void handleData(){
  float _soc=soc, _soh=soh, _bdelta=deltaSoc, _volt=voltage, _imb=imbalance;
  int _bal=balancing, _rpm=rpm, _mode=driveMode, _range=rangeKm;
  bool _fbrake=frontBrake, _rbrake=rearBrake, _hbeam=highBeam, _start=startSwitch;
  bool _kill=killSwitch, _storage=storageSwitch, _horn=horn;
  bool _indl=indicatorLeftSide, _indr=indicatorRightSide, _indc=indicatorCenterSide, _key=keyOn, _stand=sideStand;
  snprintf(jsonBuf,sizeof(jsonBuf),
    "{\"soc\":%.2f,\"soh\":%.2f,\"bdelta\":%.2f,\"volt\":%.2f,\"imb\":%.4f,\"bal\":%d,\"rpm\":%d,\"mode\":\"%s\",\"range\":%d,\"fbrake\":%s,\"rbrake\":%s,\"hbeam\":%s,\"start\":%s,\"kill\":%s,\"storage\":%s,\"horn\":%s,\"indl\":%s,\"indr\":%s,\"indc\":%s,\"key\":%s,\"stand\":%s}",
    _soc,_soh,_bdelta,_volt,_imb,_bal,_rpm,driveModeName(_mode),_range,
    _fbrake?"true":"false",_rbrake?"true":"false",_hbeam?"true":"false",_start?"true":"false",
    _kill?"true":"false",_storage?"true":"false",_horn?"true":"false",
    _indl?"true":"false",_indr?"true":"false",_indc?"true":"false",_key?"true":"false",_stand?"true":"false");
  server.send(200,"application/json",jsonBuf);
}

char cellBuf[512];
void handleCells(){
  float v[14],h[14];
  for(int i=0;i<14;i++){ v[i]=cellVoltage[i]; h[i]=cellSoh[i]; }
  char *p=cellBuf; int rem=sizeof(cellBuf);
  int n=snprintf(p,rem,"{\"v\":["); p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%.4f%s",v[i],i<13?",":""); p+=n; rem-=n; }
  n=snprintf(p,rem,"],\"h\":["); p+=n; rem-=n;
  for(int i=0;i<14;i++){ n=snprintf(p,rem,"%.0f%s",h[i],i<13?",":""); p+=n; rem-=n; }
  snprintf(p,rem,"]}");
  server.send(200,"application/json",cellBuf);
}

void setupCAN(){
  pinMode(CAN_RX, INPUT_PULLUP);
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX,(gpio_num_t)CAN_RX,TWAI_MODE_NORMAL);
  g.rx_queue_len=32; g.tx_queue_len=8;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if(twai_driver_install(&g,&t,&f)==ESP_OK){ twai_start(); Serial.println("CAN started, listening..."); }
  else Serial.println("Failed to install TWAI");
}

inline void handleFrame(const twai_message_t &m){
  const uint8_t *d=m.data;
  switch(m.identifier){
    case 0x131: soc=((d[4]+(d[5]<<8))/100.0f); if(bootSoc<0) bootSoc=soc; break;
    case 0x14D: soh=((d[4]+(d[5]*256))/100.0f); break;
    case 0x13D: deltaSoc=((d[4]+(d[5]*256))/100.0f); break;
    case 0x141: voltage=((d[0]+(d[1]*256))/100.0f); imbalance=((d[2]+(d[3]*256))/10000.0f); balancing=(d[6]+(d[7]*256)); break;
    case 0x200: rangeKm=d[6]; break;
    case 0x510: rpm=(int)(((d[2]+(d[3]*256))-(((d[3]>>7)&1)*65536))*0.833333333333f); break;
    case 0x101: driveMode=d[1]; break;
    case 0x102:
      sideStand=(d[4]&1);
      frontBrake=((d[0]>>0)&1); rearBrake=((d[0]>>1)&1); highBeam=((d[0]>>2)&1);
      startSwitch=((d[0]>>4)&1); killSwitch=((d[0]>>5)&1); storageSwitch=((d[0]>>6)&1);
      horn=((d[1]>>1)&1); indicatorLeftSide=((d[2]>>0)&1); indicatorRightSide=((d[2]>>1)&1); indicatorCenterSide=((d[2]>>2)&1); keyOn=((d[4]>>3)&1);
      break;
    case 0x132: cellVoltage[0]=(d[0]+(d[1]*256))/10000.0f; cellVoltage[1]=(d[2]+(d[3]*256))/10000.0f; cellVoltage[2]=(d[4]+(d[5]*256))/10000.0f; cellVoltage[3]=(d[6]+(d[7]*256))/10000.0f; break;
    case 0x133: cellVoltage[4]=(d[0]+(d[1]*256))/10000.0f; cellVoltage[5]=(d[2]+(d[3]*256))/10000.0f; cellVoltage[6]=(d[4]+(d[5]*256))/10000.0f; cellVoltage[7]=(d[6]+(d[7]*256))/10000.0f; break;
    case 0x134: cellVoltage[8]=(d[0]+(d[1]*256))/10000.0f; cellVoltage[9]=(d[2]+(d[3]*256))/10000.0f; cellVoltage[10]=(d[4]+(d[5]*256))/10000.0f; cellVoltage[11]=(d[6]+(d[7]*256))/10000.0f; break;
    case 0x135: cellVoltage[12]=(d[0]+(d[1]*256))/10000.0f; cellVoltage[13]=(d[2]+(d[3]*256))/10000.0f; break;
    case 0x14e: cellSoh[0]=d[0]; cellSoh[1]=d[1]; cellSoh[2]=d[2]; cellSoh[3]=d[3]; cellSoh[4]=d[4]; cellSoh[5]=d[5]; cellSoh[6]=d[6]; cellSoh[7]=d[7]; break;
    case 0x14f: cellSoh[8]=d[0]; cellSoh[9]=d[1]; cellSoh[10]=d[2]; cellSoh[11]=d[3]; cellSoh[12]=d[4]; cellSoh[13]=d[5]; break;
  }
}

void printStatus(){
  twai_status_info_t st; twai_get_status_info(&st);
  Serial.println("========== Ather OBD v2 ==========");
  Serial.printf(" SoC:%6.2f%% SoH:%6.2f%% Delta:%6.2f%%\n", soc, soh, deltaSoc);
  Serial.printf(" Volt:%6.2fV Imb:%.4fV Bal:%d RPM:%d Mode:%s Range:%dkm\n", voltage, imbalance, balancing, rpm, driveModeName(driveMode), rangeKm);
  Serial.printf(" Frames:%lu dropped:%lu bus:%d\n", (unsigned long)framesTotal, (unsigned long)st.rx_missed_count, st.state);
  Serial.println("--- Cells ---");
  for(int i=0;i<14;i++){
    Serial.printf(" C%02d: V=%6.4f SoH=%3.0f%%\n", i+1, cellVoltage[i], cellSoh[i]);
  }
}

void setup(){
  Serial.begin(115200); delay(1000);
  Serial.println("\n=== Ather OBD Scanner v2 ===");
  WiFi.persistent(false); WiFi.disconnect(true,true); delay(200);
  WiFi.mode(WIFI_AP); delay(200);
  bool apOk=WiFi.softAP(AP_SSID,AP_PASS,6,0,4); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  IPAddress ip=WiFi.softAPIP();
  Serial.printf("AP %s SSID:%s IP:%s\n", apOk?"OK":"FAIL", AP_SSID, ip.toString().c_str());
  server.on("/",handleRoot); server.on("/data",handleData); server.on("/cells",handleCells); server.begin();
  Serial.println("http://192.168.4.1/");
  setupCAN();
}

void loop(){
  server.handleClient();
  for(int i=0;i<16;i++){
    twai_message_t msg;
    if(twai_receive(&msg,0)!=ESP_OK) break;
    handleFrame(msg); framesTotal++;
    if(i%4==0) server.handleClient();
  }
  if(millis()-lastPrint>=1000){ lastPrint=millis(); printStatus(); }
}
