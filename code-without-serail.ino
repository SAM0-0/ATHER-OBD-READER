#include "driver/twai.h"
#include <WiFi.h>
#include <WebServer.h>

#define CAN_TX 10
#define CAN_RX 20
const char* AP_SSID = "ATHER-OBD";
const char* AP_PASS = "12345678";

// ===== simple state — no BMS_/BCM_ prefixes =====
volatile float soc = 0;            // SoC %
volatile float soh = 0;            // SoH %  (0x14D byte4+5)
volatile float deltaSoc = 0;       // Delta SoC % (0x13D)
volatile float voltage = 0;    // Pack voltage V (0x141)
volatile float imbalance = 0;      // Voltage imbalance V (0x141)
volatile int   balancing = 0;      // Balancing state (0x141)
volatile int   rpm = 0;            // Motor RPM (0x510)  — you wrote 'rom'
volatile int   driveMode = 0;      // 1=SPORTS 2=DRIVE 3=ECO 6=WARP 8=S.ECO
volatile bool  sideStand = false;  // was BCM_InSidestand
volatile bool  frontBrake = false, rearBrake = false, highBeam = false, startSwitch = false;
volatile bool  killSwitch = false, storageSwitch = false, horn = false;
volatile bool  indicatorLeftSide = false, indicatorRightSide = false, indicatorCenterSide = false, keyOn = false;

float bootSoc = -1;
volatile int rangeKm = 0; // VC_VehicleRange from 0x200 byte6

uint32_t framesTotal = 0;
WebServer server(80);

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
<title>Ather OBD Scanner</title>
<style>
:root{--bg:#eceeef;--card:#ffffff;--text:#2b2e34;--muted:#6b7280;--border:#e2e6ea;--accent:#5a7a6a;--accent2:#6e8298;}
*{box-sizing:border-box}
body{font-family:Inter,Arial,sans-serif;text-align:center;background:var(--bg);color:var(--text);margin:0;}

header{background:var(--card);border-bottom:1px solid var(--border);padding:16px 10px;position:relative;z-index:1;}
header h1{margin:0;font-size:24px;letter-spacing:0.3px;color:var(--text);}
.card{max-width:520px;margin:16px auto;background:var(--card);border-radius:14px;overflow:hidden;border:1px solid var(--border);box-shadow:0 2px 10px rgba(0,0,0,0.06);position:relative;z-index:1;}
.card h2{margin:0;padding:10px;background:#f3f4f5;color:var(--muted);font-size:13px;letter-spacing:0.6px;text-transform:uppercase;border-bottom:1px solid var(--border);}
table{width:100%;border-collapse:collapse;font-size:15px;}
td{padding:10px 16px;border-bottom:1px solid #f0f1f2;text-align:left;}
td:first-child{color:var(--muted);}
td.v{color:var(--accent);font-weight:700;text-align:right;}
td.on{color:var(--accent);font-weight:700;text-align:right;}
td.off{color:#9aa0a8;text-align:right;}
</style>
<script>
function sw(id,val,onTxt,offTxt){let e=document.getElementById(id);e.innerText=val?onTxt:offTxt;e.className=val?'on':'off';}
async function update(){
 try{
  let r=await fetch('/data'); let j=await r.json();
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
setInterval(update,1000); window.onload=update;
</script></head><body>
<header><h1>⚡ Ather OBD Scanner</h1><p style="margin:6px 0 0;color:var(--muted);font-size:13px;">Developed by <b style="color:var(--text);">sam0_0</b> • Ather OBD Scanner</p><p style="margin:10px 0 0;"><a href="https://www.buymeacoffee.com/sam0_0" target="_blank" style="display:inline-block;background:#ffdd00;color:#1a1a1a;padding:7px 16px;border-radius:999px;font-size:13px;font-weight:700;text-decoration:none;box-shadow:0 2px 6px rgba(0,0,0,0.12);">☕ Support this project — Buy Me a Coffee</a></p></header>
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
<footer style="margin:18px auto 24px;max-width:520px;color:#8a9099;font-size:12px;opacity:0.85;">Developer: <b style="color:#5a7a6a;">sam0_0</b> • Ather OBD Scanner</footer>
</body></html>
)HTML";

void handleRoot(){ server.send(200,"text/html",htmlPage); }
char jsonBuf[768];
void handleData(){
  float _soc=soc, _soh=soh, _bdelta=deltaSoc, _volt=voltage, _imb=imbalance;
  int _bal=balancing, _rpm=rpm, _mode=driveMode, _range=rangeKm;
  bool _fbrake=frontBrake, _rbrake=rearBrake, _hbeam=highBeam, _start=startSwitch;
  bool _kill=killSwitch, _storage=storageSwitch, _horn=horn;
  bool _indl=indicatorLeftSide, _indr=indicatorRightSide, _indc=indicatorCenterSide, _key=keyOn, _stand=sideStand;
  int n=snprintf(jsonBuf,sizeof(jsonBuf),
    "{\"soc\":%.2f,\"soh\":%.2f,\"bdelta\":%.2f,\"volt\":%.2f,\"imb\":%.4f,\"bal\":%d,\"rpm\":%d,\"mode\":\"%s\",\"range\":%d,\"fbrake\":%s,\"rbrake\":%s,\"hbeam\":%s,\"start\":%s,\"kill\":%s,\"storage\":%s,\"horn\":%s,\"indl\":%s,\"indr\":%s,\"indc\":%s,\"key\":%s,\"stand\":%s}",
    _soc,_soh,_bdelta,_volt,_imb,_bal,_rpm,driveModeName(_mode),_range,
    _fbrake?"true":"false",_rbrake?"true":"false",_hbeam?"true":"false",_start?"true":"false",
    _kill?"true":"false",_storage?"true":"false",_horn?"true":"false",
    _indl?"true":"false",_indr?"true":"false",_indc?"true":"false",_key?"true":"false",_stand?"true":"false");
  server.send(200,"application/json",jsonBuf); (void)n;
}

void setupCAN(){
  pinMode(CAN_RX, INPUT_PULLUP);
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX,(gpio_num_t)CAN_RX,TWAI_MODE_NORMAL);
  g.rx_queue_len=32; g.tx_queue_len=8;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if(twai_driver_install(&g,&t,&f)==ESP_OK){ twai_start(); }
}
inline void handleFrame(const twai_message_t &m){
  const uint8_t *d=m.data;
  switch(m.identifier){
    case 0x131: soc=((d[4]+(d[5]<<8))/100.0f); if(bootSoc<0) bootSoc=soc; break;
    case 0x14D: soh=((d[4]+(d[5]*256))/100.0f); break;
    case 0x13D: deltaSoc=((d[4]+(d[5]*256))/100.0f); break;
    case 0x141: voltage=((d[0]+(d[1]*256))/100.0f); imbalance=((d[2]+(d[3]*256))/10000.0f); balancing=(d[6]+(d[7]*256)); break;
    case 0x200: rangeKm=d[6]; break; // VC_VehicleRange: byte6
    case 0x510: rpm=(int)(((d[2]+(d[3]*256))-(((d[3]>>7)&1)*65536))*0.833333333333f); break;
    case 0x101: driveMode=d[1]; break;
    case 0x102:
      sideStand=(d[4]&1);
      frontBrake=((d[0]>>0)&1); rearBrake=((d[0]>>1)&1); highBeam=((d[0]>>2)&1);
      startSwitch=((d[0]>>4)&1); killSwitch=((d[0]>>5)&1); storageSwitch=((d[0]>>6)&1);
      horn=((d[1]>>1)&1); indicatorLeftSide=((d[2]>>0)&1); indicatorRightSide=((d[2]>>1)&1); indicatorCenterSide=((d[2]>>2)&1); keyOn=((d[4]>>3)&1);
      break;
  }
}
void setup(){
  WiFi.persistent(false); WiFi.disconnect(true,true); delay(200);
  WiFi.mode(WIFI_AP); delay(200);
  WiFi.softAP(AP_SSID,AP_PASS,6,0,4); WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.on("/",handleRoot); server.on("/data",handleData); server.begin();
  setupCAN();
}
void loop(){
  server.handleClient();
  // drain max 8 frames per loop to keep WiFi responsive on single-core C3
  for(int i=0;i<8;i++){
    twai_message_t msg;
    if(twai_receive(&msg,0)!=ESP_OK) break;
    handleFrame(msg); framesTotal++;
    if(i%4==0) server.handleClient(); // yield to HTTP mid-burst
  }
  // CRITICAL on single-core C3: loop() and the WiFi/lwIP stack share one core.
  // Without ever yielding, the scheduler can starve WiFi tasks intermittently —
  // that's the "loads forever / shows --" behavior you saw. This delay(1) costs
  // ~1ms, during which the 32-deep TWAI rx queue easily absorbs incoming frames,
  // so it doesn't cause frame loss — but it lets WiFi run reliably every pass.
  delay(1);
}
