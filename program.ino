#include <ETH.h>
#include <WiFiUdp.h>
#include <vector>
#include <map>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include "esp_event.h"
#include "esp_eth.h"
#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include <EEPROM.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <string.h>   // memset, strncpy, strncmp
#include "esp_system.h"
// ======================================================
// =============== USER CONFIG (edit here) ==============
// ======================================================
String version = "0.90";
// ======================================================
// --- SD_MMC runtime ---
static bool   g_sd_mounted = false;
static String g_sd_logfile = "";     // vybraný log súbor pre tento boot
const uint32_t SD_MIN_FREE_KB = 64;  // minimálna rezerva (upráv podľa chuti)


String  IP_assign_for_all = "manual";         // "auto" (DHCP) alebo "manual"
String  my_IP            = "192.168.0.10";  // pri manual
String  role             = "master";        // "master" alebo "slave"

int     slave_count      = 1;
String  master_IP        = "192.168.0.10";
String  slave1_IP        = "192.168.0.105";
String  slave2_IP        = "192.168.0.106";

int     udp_port         = 5005;
int     tcp_port         = 1025;

bool    device_override  = false;           // rezervované do budúcna
String  localGateway     = "192.168.0.1";
String  localSubnet      = "255.255.255.0";
String  UDP_Broadcast_IP_selection = "auto"; // "auto" alebo "manual"
String  UDP_Broadcast_IP = "192.168.0.255";  // altern. "255.255.255.255"

static  bool eth_connected = false;

// ======================================================
// ================== GLOBAL STATE FLAGS ===============
// ======================================================
bool        setupMode_ON           = false;
static bool LAN_initialized        = false;
bool        slaveHandshakeDone     = false;
bool        slaveDiscoveredByMaster= false; // slave už odpovedal na master UDP

// ======================================================
// ================== HARDWARE PINS =====================
// ======================================================
// MASTER: START (NO, externý pull-up, stlačené = LOW), E-STOP (NC ku GND, pull-up, stlačené = HIGH)
const int PIN_START    = 36;   // MASTER ONLY
const int PIN_ESTOP    = 13;   // MASTER ONLY
// SLAVE: relé aktívne v LOW (LOW = ON)
const int PIN_RELAY    = 4;    // SLAVE ONLY
// Info LED: zapojená na +3.3V -> ak má svietiť, pin ťahá do GND (aktívna LOW)
const int PIN_LED      = 5;
// Setup prepínač
const int PIN_SETUP_SW = 16;
// PHY reset pin (Olimex ESP32-POE)
const int PIN_PHY_RST  = 12;
// Rezervný tlačidlový vstup na PCB
const int PCB_Button   = 34;

// LED aktívna v LOW (svietenie = pin LOW)
const bool LED_ACTIVE_HIGH = false;
inline void ledOn()  { digitalWrite(PIN_LED, LED_ACTIVE_HIGH ? HIGH : LOW); }
inline void ledOff() { digitalWrite(PIN_LED, LED_ACTIVE_HIGH ? LOW  : HIGH); }

// Relé aktívne v LOW (zapnúť = pin LOW)
const bool RELAY_ACTIVE_LOW = true;

// ======================================================
// ===================== MODES / LED ====================
// ======================================================
// Pôvodne: enum Mode { MODE_WIFI_SETUP, MODE_BOOT, MODE_SEARCH, MODE_WORK };
enum Mode { MODE_WIFI_SETUP, MODE_BOOT, MODE_SEARCH, MODE_WORK };

Mode currentMode = MODE_BOOT;

struct BlinkPattern { unsigned long on_ms; unsigned long off_ms; };
// setup: krátke bliknutie raz za sekundu (opticky “setup-only”)
BlinkPattern pattern_setup        = { 100, 900 };
BlinkPattern pattern_boot_search  = { 150, 150 };
BlinkPattern pattern_work         = { ULONG_MAX, 0 }; // steady ON

// ======================================================
/* =================== NETWORK OBJECTS ==================
   ETH (drôt), UDP (broadcast discovery), TCP (riadenie) */
WiFiUDP   udp;
IPAddress broadcastIP;
IPAddress staticIP, gwIP, subnetIP;

std::vector<IPAddress> slaveIPsManual;     // manuálne zadané
std::vector<IPAddress> slaveIPsDiscovered; // zistené cez UDP (AUTO)

// TCP peers
struct Peer {
  IPAddress ip;
  int sockfd;
  bool connected;
  unsigned long lastHeartbeat;  // millis()
  bool confirmedWaitingForAll;  // handshake complete
  String rx;                    // buffer na parsovanie riadkov (správy ukončené '\n')
};
std::map<String, Peer> peers;   // key = ip string
// --- Global I/O buffers (shared by UDP/TCP readers) ---
const int BUF_SZ = 256;
char buf[BUF_SZ];

// --- Slave TCP server globals ---
int  slave_listen_sock   = -1;
bool slave_server_running = false;

// wifi_setp mod vypina siet lan:
static bool eth_events_suppressed = false;
// ======================================================
// ================= TIMERS / HEARTBEATS ================
// ======================================================
unsigned long lastLedToggle     = 0;
bool          ledState          = false;
unsigned long lastHeartbeatSend = 0;

const unsigned long HEARTBEAT_INTERVAL = 1000; // 1s
const unsigned long COMMS_TIMEOUT      = 4000; // 4s

// Debounce pre tlačidlá (MASTER)
unsigned long lastDebounceTimeStart = 0;
unsigned long lastDebounceTimeEstop = 0;
const unsigned long DEBOUNCE_MS     = 50;

// ======================================================
// =================== FORWARD DECLS ====================
// ======================================================
// lifecycle
void enterMode(Mode m);
void runWifiSetupMode();
void runBootMode();
void runSearchMode();
void runWorkMode();

// hw helpers
void doLedBlink();
void checkSetupSwitch();
void phyResetPulse();

// ETH / IP helpers (zgrupené “sieťové helpery”)
void setupETH();
void ethEventHandler(void*, esp_event_base_t, int32_t, void*);
void ethGotIpHandler(void*, esp_event_base_t, int32_t, void*);
bool isIPValid(IPAddress ip);
bool ensureIPAvailable();
bool ensureLinkAndIPReady();
void resetSlaveDiscoveryState();

// UDP discovery helpers
bool masterUdpDiscovery(unsigned long timeoutMs = 5000);
bool slaveUdpListenAndReply(unsigned long timeoutMs = 5000);

// TCP helpers
void startSlaveTcpServer();          // slave: listen
bool slaveTcpAcceptLoop();           // slave: accept+handshake
void masterTcpConnectAll();          // master: connect to all targets
void masterHandleClients();          // master: recv from peers
int  makeNonBlocking(int fd);
void closePeer(Peer &p);
void closeAllPeers();
void broadcastToPeers(const String &msg);
void sendHeartbeatsIfNeeded();
bool dropIfTimedOut();

// IO helpers
String ipToStr(IPAddress ip);
static inline bool readStartRaw()  { return digitalRead(PIN_START) == LOW; }   // NO, pull-up → pressed = LOW
static inline bool readEstopRaw()  { return digitalRead(PIN_ESTOP) == HIGH; }  // NC+pull-up → pressed/open = HIGH
//void debouncedInputs(bool &startPressed, bool &estopActive);
void instantDebounce(bool &startPressed, bool &estopActive); 
void setRelay(bool on);

bool write_SD(bool first_write_after_boot, const String& text);
// ====== EEPROM CONFIG ======
#define EEPROM_SIZE  512       // stačí do 512 B
#define EEPROM_MAGIC "kvant123"// sentinel marker

struct Config {
  char magic[16];              // "kvant123" => platná EEPROM
  char ipAssign[8];            // "auto"/"manual"
  char myIP[16];
  char role[8];                // "master"/"slave"
  uint8_t slave_count;
  char masterIP[16];
  char slave1IP[16];
  char slave2IP[16];
  uint16_t udp_port;
  uint16_t tcp_port;
  bool device_override;
  char gateway[16];
  char subnet[16];
  char udpBcastSel[8];         // "auto"/"manual"
  char udpBcastIP[16];
};
Config g_cfg;

// forwardy
void cfgDefaults();                // nastav default do g_cfg
void cfgLoadOrDefaults();          // načítaj z EEPROM alebo default
void cfgSave();                    // ulož g_cfg do EEPROM
void cfgToGlobals();               // g_cfg -> tvoje globálne premenné
void globalsToCfg();               // globálne premenné -> g_cfg
// ======================================================
// =============== Captive portal wifi===================
// ======================================================
// zavri všetko čo súvisí s LAN (UDP/TCP/ETH)
void shutdownLANForSetup() {
  Serial.println("WIFI_SETUP: shutting down LAN (sockets + ETH)");

  // 1) UDP/TCP
  udp.stop();
  closeAllPeers();
  if (slave_listen_sock >= 0) { close(slave_listen_sock); slave_listen_sock = -1; }
  slave_server_running = false;

  // 2) Stop ETH eventy – odregistrovať handlery (bez paniky, ak už nie sú registrované)
  eth_events_suppressed = true;
  esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethGotIpHandler);

  // 3) Fyzicky stiahnuť PHY do resetu (link down)
  pinMode(PIN_PHY_RST, OUTPUT);
  digitalWrite(PIN_PHY_RST, LOW);

  // 4) (Voliteľné) vypni WiFi STA aby bežal len AP
  WiFi.mode(WIFI_AP);
}

// ====== Captive Portal (WIFI_SETUP) ======
const char* AP_PASS = "kvant123";      // hardcoded (NEUKLADÁ sa do EEPROM)
DNSServer dnsServer;
WebServer web(80);
IPAddress apIP(192,168,4,1), apGW(192,168,4,1), apMS(255,255,255,0);

String apSSID() {
  uint8_t mac[6];
  WiFi.softAPmacAddress(mac);  // SoftAP MAC (správne pre AP)
  char ssid[32];
  // tvar: ESP32-C3-30-97 (posledné 2 bajty, ako chceš)
   snprintf(ssid, sizeof(ssid), "ESP32-%02X-%02X-%02X", mac[3], mac[4], mac[5]);
  return String(ssid);
}

String htmlPage(const String& body) {
  return String(F(
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Setup</title>"
    "<style>"
      "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:0;background:#f5f7fb;}"
      ".wrap{max-width:560px;margin:6vh auto;background:#fff;padding:24px;border-radius:16px;box-shadow:0 6px 24px rgba(0,0,0,.08);}"
      "h1{font-size:22px;margin:0 0 6px}"
      ".ver{color:#6b7280;font-size:12px;margin:0 0 12px}"
      "label{display:block;margin:10px 0 6px;color:#333}"
      "input,select{width:100%;padding:10px 12px;border:1px solid #ccd3e0;border-radius:10px;font-size:14px}"
      "button{margin-top:16px;width:100%;padding:12px 14px;border:0;border-radius:12px;background:#0b72ff;color:#fff;font-weight:600;cursor:pointer}"
      ".ok{color:#0a7e2a;margin:14px 0;font-weight:600;text-align:center}"
      "a.btn{display:inline-block;margin-top:12px;padding:10px 14px;border-radius:10px;background:#eef4ff;color:#0b72ff;text-decoration:none}"
      ".ro{background:#eee;color:#666}"
      ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
    "</style>"
    "<script>"
      "function ipOk(v){"
        "if(!v) return true;"
        "var m=v.match(/^\\s*(\\d+)\\.(\\d+)\\.(\\d+)\\.(\\d+)\\s*$/);"
        "if(!m) return false;"
        "for(var i=1;i<=4;i++){ var n=+m[i]; if(n<0||n>255) return false; }"
        "return true;"
      "}"
      "function applyMode(){"
        "var manual = (document.getElementById('ipAssign').value === 'manual');"
        "['myIP','gw','mask','mip','s1','s2','uport','tport'].forEach(function(id){"
          "var el=document.getElementById(id); if(!el) return;"
          "el.readOnly = !manual;"
          "el.classList.toggle('ro', !manual);"
        "});"
        "var bselManual = (document.getElementById('bsel').value === 'manual');"
        "var bip=document.getElementById('bip');"
        "if(bip){ bip.readOnly = !bselManual; bip.classList.toggle('ro', !bselManual); }"
      "}"
      "function validateForm(evt){"
        "var ids=['myIP','gw','mask','mip','s1','s2','bip'];"
        "var bad=[];"
        "ids.forEach(function(id){"
          "var el=document.getElementById(id); if(!el) return;"
          "var v=el.value;"
          "if(v && !ipOk(v)) bad.push(id);"
        "});"
        "if(bad.length){"
          "alert('Some IP fields are invalid: '+bad.join(', '));"
          "evt.preventDefault(); return false;"
        "}"
        "return true;"
      "}"
      "document.addEventListener('DOMContentLoaded',function(){"
        "applyMode();"
        "var f=document.getElementById('f'); if(f) f.addEventListener('submit', validateForm);"
        "var selA=document.getElementById('ipAssign'); if(selA) selA.addEventListener('change', applyMode);"
        "var selB=document.getElementById('bsel'); if(selB) selB.addEventListener('change', applyMode);"
      "});"
    "</script>"
    "</head><body><div class='wrap'>"
  )) + body + F("</div></body></html>");
}

void handleRoot() {
  String ssid = apSSID();      // napr. "ESP32-C33097"
  String b;

  b += F("<h1>Device Setup (");
  b += ssid;
  b += F(")</h1>");

  // verzia pod nadpisom (premenná: String version = \"0.90\")
  b += F("<div class='ver'>Version ");
  b += version;
  b += F("</div>");

  b += F("<form id='f' method='POST' action='/save'>");

  // Role
  b += F("<label>Role</label><select name='role'>");
  b += F("<option");
  if (role=="master") b += F(" selected");
  b += F(">master</option>");
  b += F("<option");
  if (role=="slave") b += F(" selected");
  b += F(">slave</option></select>");

  // IP assign
  b += F("<label>IP assign</label><select id='ipAssign' name='ipAssign'>");
  b += F("<option");
  if (IP_assign_for_all=="auto") b += F(" selected");
  b += F(">auto</option>");
  b += F("<option");
  if (IP_assign_for_all=="manual") b += F(" selected");
  b += F(">manual</option></select>");

  // My IP, GW, Subnet
  b += F("<label>My IP</label><input id='myIP' name='myIP' value='");
  b += my_IP;
  b += F("'>");

  b += F("<label>Gateway</label><input id='gw' name='gw' value='");
  b += localGateway;
  b += F("'>");

  b += F("<label>Subnet</label><input id='mask' name='mask' value='");
  b += localSubnet;
  b += F("'>");

  // Master + Slaves
  b += F("<label>Master IP</label><input id='mip' name='mip' value='");
  b += master_IP;
  b += F("'>");

  b += F("<label>Slave1 IP</label><input id='s1' name='s1' value='");
  b += slave1_IP;
  b += F("'>");

  b += F("<label>Slave2 IP</label><input id='s2' name='s2' value='");
  b += slave2_IP;
  b += F("'>");

  // Slave count (select 1–2)
  b += F("<label>Slave count</label><select name='sc'>");
  b += F("<option value='1'");
  if (slave_count==1) b += F(" selected");
  b += F(">1</option>");
  b += F("<option value='2'");
  if (slave_count==2) b += F(" selected");
  b += F(">2</option></select>");

  // UDP/TCP ports (sive v AUTO)
  b += F("<label>UDP port</label><input id='uport' name='uport' type='number' value='");
  b += String(udp_port);
  b += F("'>");

  b += F("<label>TCP port</label><input id='tport' name='tport' type='number' value='");
  b += String(tcp_port);
  b += F("'>");

  // UDP broadcast selection + IP
  b += F("<label>UDP broadcast IP selection</label><select id='bsel' name='bsel'>");
  b += F("<option");
  if (UDP_Broadcast_IP_selection=="auto") b += F(" selected");
  b += F(">auto</option>");
  b += F("<option");
  if (UDP_Broadcast_IP_selection=="manual") b += F(" selected");
  b += F(">manual</option></select>");

  b += F("<label>UDP Broadcast IP</label><input id='bip' name='bip' value='");
  b += UDP_Broadcast_IP;
  b += F("'>");

  // Device override
  b += F("<label>Device override (offline)</label><select name='ovr'>");
  b += F("<option value='0'");
  if (!device_override) b += F(" selected");
  b += F(">false</option>");
  b += F("<option value='1'");
  if (device_override) b += F(" selected");
  b += F(">true</option></select>");

  b += F("<button type='submit'>Save</button></form>");

  web.send(200, "text/html", htmlPage(b));
}





// po uložení
void handleSave() {
  // čítanie z POST a aktualizácia globálov
  role = web.arg("role");
  IP_assign_for_all = web.arg("ipAssign");
  my_IP = web.arg("myIP");
  localGateway = web.arg("gw");
  localSubnet  = web.arg("mask");
  master_IP    = web.arg("mip");
  slave1_IP    = web.arg("s1");
  slave2_IP    = web.arg("s2");
  UDP_Broadcast_IP_selection = web.arg("bsel");
  UDP_Broadcast_IP = web.arg("bip");
  udp_port = (uint16_t) web.arg("uport").toInt();
  tcp_port = (uint16_t) web.arg("tport").toInt();
  slave_count = (uint8_t) web.arg("sc").toInt();
  device_override = (web.arg("ovr") == "1");

  cfgSave();                     // uložiť do EEPROM
  write_SD(false, "cfg-save");   // voliteľný log

  String b;
  b += F("<h1>Save success</h1><p class='ok'>Configuration saved to EEPROM.</p>");
  b += F("<a class='btn' href='/'>Back to setup</a>");
  web.send(200, "text/html", htmlPage(b));
}

// presmerovanie všetkého na root (captive)
void handleCaptive() {
  web.sendHeader("Location", String("http://") + apIP.toString() + "/", true);
  web.send(302, "text/plain", "");
}

void startCaptivePortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apGW, apMS);
  String ssid = apSSID();
  WiFi.softAP(ssid.c_str(), AP_PASS);  // password hardcoded

  // DNS: presmeruj všetko na 192.168.4.1
  dnsServer.start(53, "*", apIP);

  // Web
  web.on("/", HTTP_GET, handleRoot);
  web.on("/save", HTTP_POST, handleSave);
  web.onNotFound(handleCaptive);
  web.begin();

  Serial.print("AP SSID: "); Serial.println(ssid);
  Serial.print("AP IP  : "); Serial.println(apIP);
}

void stopCaptivePortal() {
  web.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}
// ======================================================
// ================== eeprom  ===========================
// ======================================================
static void strzcpy(char* dst, const String& src, size_t cap) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = 0;
}

void cfgDefaults() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  strzcpy(g_cfg.magic, EEPROM_MAGIC, sizeof(g_cfg.magic));
  strzcpy(g_cfg.ipAssign, IP_assign_for_all, sizeof(g_cfg.ipAssign)); // použijeme tvoje aktuálne defaulty
  strzcpy(g_cfg.myIP, my_IP, sizeof(g_cfg.myIP));
  strzcpy(g_cfg.role, role, sizeof(g_cfg.role));
  g_cfg.slave_count = slave_count;
  strzcpy(g_cfg.masterIP, master_IP, sizeof(g_cfg.masterIP));
  strzcpy(g_cfg.slave1IP, slave1_IP, sizeof(g_cfg.slave1IP));
  strzcpy(g_cfg.slave2IP, slave2_IP, sizeof(g_cfg.slave2IP));
  g_cfg.udp_port = udp_port;
  g_cfg.tcp_port = tcp_port;
  g_cfg.device_override = device_override;
  strzcpy(g_cfg.gateway, localGateway, sizeof(g_cfg.gateway));
  strzcpy(g_cfg.subnet, localSubnet, sizeof(g_cfg.subnet));
  strzcpy(g_cfg.udpBcastSel, UDP_Broadcast_IP_selection, sizeof(g_cfg.udpBcastSel));
  strzcpy(g_cfg.udpBcastIP, UDP_Broadcast_IP, sizeof(g_cfg.udpBcastIP));
}

void cfgToGlobals() {
  IP_assign_for_all          = String(g_cfg.ipAssign);
  my_IP                      = String(g_cfg.myIP);
  role                       = String(g_cfg.role);
  slave_count                = g_cfg.slave_count;
  master_IP                  = String(g_cfg.masterIP);
  slave1_IP                  = String(g_cfg.slave1IP);
  slave2_IP                  = String(g_cfg.slave2IP);
  udp_port                   = g_cfg.udp_port;
  tcp_port                   = g_cfg.tcp_port;
  device_override            = g_cfg.device_override;
  localGateway               = String(g_cfg.gateway);
  localSubnet                = String(g_cfg.subnet);
  UDP_Broadcast_IP_selection = String(g_cfg.udpBcastSel);
  UDP_Broadcast_IP           = String(g_cfg.udpBcastIP);
}

void globalsToCfg() {
  strzcpy(g_cfg.ipAssign, IP_assign_for_all, sizeof(g_cfg.ipAssign));
  strzcpy(g_cfg.myIP, my_IP, sizeof(g_cfg.myIP));
  strzcpy(g_cfg.role, role, sizeof(g_cfg.role));
  g_cfg.slave_count = slave_count;
  strzcpy(g_cfg.masterIP, master_IP, sizeof(g_cfg.masterIP));
  strzcpy(g_cfg.slave1IP, slave1_IP, sizeof(g_cfg.slave1IP));
  strzcpy(g_cfg.slave2IP, slave2_IP, sizeof(g_cfg.slave2IP));
  g_cfg.udp_port = udp_port;
  g_cfg.tcp_port = tcp_port;
  g_cfg.device_override = device_override;
  strzcpy(g_cfg.gateway, localGateway, sizeof(g_cfg.gateway));
  strzcpy(g_cfg.subnet, localSubnet, sizeof(g_cfg.subnet));
  strzcpy(g_cfg.udpBcastSel, UDP_Broadcast_IP_selection, sizeof(g_cfg.udpBcastSel));
  strzcpy(g_cfg.udpBcastIP, UDP_Broadcast_IP, sizeof(g_cfg.udpBcastIP));
}

void cfgLoadOrDefaults() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, g_cfg);

  // bezpečná kontrola: porovnaj len dĺžku literálu "kvant123"
  const size_t mlen = strlen(EEPROM_MAGIC);
  if (mlen == 0 || strncmp(g_cfg.magic, EEPROM_MAGIC, mlen) != 0) {
    Serial.println("EEPROM: invalid/empty -> using DEFAULTS");
    cfgDefaults();
    cfgSave();
  } else {
    Serial.println("EEPROM: loaded OK");
  }
  cfgToGlobals();
}


void cfgSave() {
  globalsToCfg(); // najprv zosúladíme g_cfg s runtime premennými
  // uisti sa, že magic ostáva správny
  strzcpy(g_cfg.magic, EEPROM_MAGIC, sizeof(g_cfg.magic));
  EEPROM.put(0, g_cfg);
  EEPROM.commit();
  Serial.println("EEPROM: saved");
}


// ======================================================
// ====================== SETUP/LOOP ====================
// ======================================================
void setup() {
  delay(500);
  Serial.begin(115200);
  Serial.println("=== ESP32-POE Master/Slave Controller ===");
  Serial.println("Starting...");

  // 1) načítaj konfiguráciu (alebo default)
  cfgLoadOrDefaults();

  pinMode(PIN_START,    INPUT);
  pinMode(PIN_ESTOP,    INPUT);
  pinMode(PIN_RELAY,    OUTPUT);
  pinMode(PIN_LED,      OUTPUT);
  pinMode(PIN_SETUP_SW, INPUT);
  pinMode(PIN_PHY_RST,  OUTPUT);
  pinMode(PCB_Button,   INPUT);

  // LED default OFF, Relé default OFF
  ledOff();
  if (RELAY_ACTIVE_LOW) digitalWrite(PIN_RELAY, HIGH); else digitalWrite(PIN_RELAY, LOW);

   Serial.print(F("FW version: "));
   Serial.println(version);

    write_SD(true, "boot-ok role=" + role); // dalsie pouzitie uz len  >>   write_SD(false, "text");
    Serial.println("Writing to SD card if available...");

  // --- OFFLINE režim cez device_override ---
  if (device_override) {
  Serial.println("device_override=TRUE -> OFFLINE režim (LAN vypnuta).");
  if (role == "slave") {
    if (RELAY_ACTIVE_LOW) digitalWrite(PIN_RELAY, LOW); else digitalWrite(PIN_RELAY, HIGH);
    Serial.println("SLAVE: relay FORCED ON (offline).");
  }
  enterMode(MODE_WIFI_SETUP);
  runWifiSetupMode();
  return;                 // <<< pridaj
  }

  // hneď skús SETUP prepínač
  checkSetupSwitch();
  if (currentMode == MODE_WIFI_SETUP)  runWifiSetupMode();
  else enterMode(MODE_BOOT);


}

void loop() {
  checkSetupSwitch();

  switch (currentMode) {
    case MODE_WIFI_SETUP:  runWifiSetupMode();  break;
    case MODE_BOOT:   runBootMode();   break;
    case MODE_SEARCH: runSearchMode(); break;
    case MODE_WORK:   runWorkMode();   break;
  }

  doLedBlink();
}

// ======================================================
// ================== MODE MANAGEMENT ===================
// ======================================================
void enterMode(Mode m) {
  currentMode = m;
  switch (m) {
    case MODE_WIFI_SETUP:
      Serial.println("ENTER MODE: WIFI_SETUP");
      break;
    case MODE_BOOT:
      Serial.println("ENTER MODE: BOOT");
      break;
    case MODE_SEARCH:
      Serial.println("ENTER MODE: SEARCH");
      if (role == "slave") resetSlaveDiscoveryState(); // vždy znovu čakaj na UDP
      closeAllPeers();      // očisti TCP
      ledState = false;     // povol blikací vzor
      break;
    case MODE_WORK:
      Serial.println("ENTER MODE: WORK");
      ledOn(); ledState = true; // v WORK svieti
      break;
  }
}

// ======================================================
// ================== LED / BUTTON HELPERS ==============
// ======================================================
void doLedBlink() {
  unsigned long now = millis();
  BlinkPattern p = (currentMode == MODE_WIFI_SETUP) ? pattern_setup
                    : (currentMode == MODE_BOOT || currentMode == MODE_SEARCH) ? pattern_boot_search
                    : pattern_work;

  if (p.on_ms == ULONG_MAX) { // steady on
    if (!ledState) { ledOn(); ledState = true; }
    return;
  }

  if (ledState) {
    if (now - lastLedToggle >= p.on_ms) {
      ledState = false; ledOff(); lastLedToggle = now;
    }
  } else {
    if (now - lastLedToggle >= p.off_ms) {
      ledState = true;  ledOn();  lastLedToggle = now;
    }
  }
}

void checkSetupSwitch() {
  static bool prev = HIGH;
  bool now = digitalRead(PIN_SETUP_SW);
  if (prev == HIGH && now == LOW && currentMode != MODE_WIFI_SETUP) {
    delay(30); // proti falošnému prechodovému ruchu na drôte
    if (digitalRead(PIN_SETUP_SW) == LOW) {
      Serial.println("SETUP switch pressed -> entering WIFI_SETUP mode.");
      enterMode(MODE_WIFI_SETUP);
    }
  }
  prev = now;
}



// Reaguje okamžite na prvú hranu, potom ignoruje bouncy po dobu DEBOUNCE_MS.
// Polarity:
// - START = ACTIVE LOW (stlačené = LOW -> startPressed = true)
// - ESTOP = ACTIVE HIGH (stlačené/otvorené = HIGH -> estopActive = true)
void instantDebounce(bool &startPressed, bool &estopActive) {
  unsigned long now = millis();

  // výstupné latche (posledný "reportovaný" stav)
  static bool startOut  = false;
  static bool estopOut  = false;

  // do kedy ignorujeme klepanie pre dané tlačidlo
  static unsigned long startBlockUntil = 0;
  static unsigned long estopBlockUntil = 0;

  // načítaj RAW (už so správnou polaritou)
  bool sRaw = (digitalRead(PIN_START) == LOW);  // pressed = true
  bool eRaw = (digitalRead(PIN_ESTOP) == HIGH); // pressed/open = true

  // START: ak nie sme v blokovaní a stav sa zmenil, prevezmi okamžite a zablokuj bouncy
  if (now >= startBlockUntil && sRaw != startOut) {
    startOut = sRaw;
    startBlockUntil = now + DEBOUNCE_MS;   // typicky 20–50 ms
  }

  // ESTOP: rovnaká stratégia (okamžitá reakcia), zvyčajne chceme krátky debounce, ale stále hneď
  if (now >= estopBlockUntil && eRaw != estopOut) {
    estopOut = eRaw;
    estopBlockUntil = now + DEBOUNCE_MS;
  }

  startPressed = startOut;
  estopActive  = estopOut;
}



//---------------------------
void setRelay(bool on) {
  if (RELAY_ACTIVE_LOW) digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  else                  digitalWrite(PIN_RELAY, on ? HIGH: LOW );
}

// ======================================================
// ================== NETWORK HELPERS ===================
// ======================================================
void phyResetPulse() {
  digitalWrite(PIN_PHY_RST, LOW);  delay(100);
  digitalWrite(PIN_PHY_RST, HIGH); delay(300);
}

void ethEventHandler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
  if (eth_events_suppressed) return;   // <<< pridaj
  if (event_base == ETH_EVENT) {
    switch (event_id) {
      case ETHERNET_EVENT_CONNECTED:    Serial.println("ETH Connected"); break;
      case ETHERNET_EVENT_DISCONNECTED:
        Serial.println("ETH Disconnected");
        eth_connected = false;
        closeAllPeers();
        if (role == "slave") { setRelay(false); enterMode(MODE_SEARCH); }
        if (role == "master") { enterMode(MODE_SEARCH); }
        break;
      default: break;
    }
  }
}

void ethGotIpHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (eth_events_suppressed) return;   // <<< pridaj
  if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    IPAddress ip = IPAddress(event->ip_info.ip.addr);
    Serial.print("ETH GOT IP: "); Serial.println(ip);
    eth_connected = true;
    if (role == "slave" && currentMode != MODE_SEARCH) enterMode(MODE_SEARCH);
  }
}


void setupETH() {
  Serial.println("BOOT: PHY reset pulse...");
  phyResetPulse();
  Serial.println("BOOT: calling ETH.begin()");

  ETH.begin(ETH_PHY_LAN8720, 0, 23, 18, 12, ETH_CLOCK_GPIO17_OUT);

  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &ethEventHandler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &ethGotIpHandler, NULL));
  delay(300);
}

bool isIPValid(IPAddress ip) {
  return ip != IPAddress(0,0,0,0) && ip != IPAddress(255,255,255,255);
}

bool ensureIPAvailable() {
  IPAddress ip = ETH.localIP();
  if (isIPValid(ip)) {
    Serial.print("BOOT: current IP from ETH.localIP() is "); Serial.println(ipToStr(ip));
    return true;
  }
  delay(200);
  return isIPValid(ETH.localIP());
}

bool ensureLinkAndIPReady() {
  return ETH.linkUp() && isIPValid(ETH.localIP());
}

void resetSlaveDiscoveryState() {
  slaveDiscoveredByMaster = false;
  slaveHandshakeDone = false;
}

// ======================================================
// =============== UDP DISCOVERY HELPERS ================
// ======================================================

// MASTER: vysiela DISCOVER_MASTER a zbiera SLAVE_IP odpovede
bool masterUdpDiscovery(unsigned long timeoutMs) {
  slaveIPsDiscovered.clear();

  IPAddress myIP = ETH.localIP();
  IPAddress mask = ETH.subnetMask();

  if (UDP_Broadcast_IP_selection == "auto") broadcastIP = (myIP & mask) | (~mask);
  else if (!broadcastIP.fromString(UDP_Broadcast_IP)) {
    Serial.println("ERROR: invalid UDP_Broadcast_IP string");
    return false;
  }

  if (!udp.begin(udp_port)) { Serial.println("UDP: failed to begin port"); return false; }

  unsigned long start = millis();
  char lbuf[256];

  while (millis() - start < timeoutMs) {
    // broadcast DISCOVER_MASTER
    String msg = "DISCOVER_MASTER:" + ipToStr(myIP);
    udp.beginPacket(broadcastIP, udp_port);
    udp.write((const uint8_t*)msg.c_str(), msg.length());
    udp.endPacket();

    // collect replies
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      int len = udp.read(lbuf, sizeof(lbuf) - 1);
      if (len > 0) {
        lbuf[len] = 0;
        String payload = String(lbuf);
        if (payload.startsWith("SLAVE_IP:")) {
          IPAddress sip; sip.fromString(payload.substring(9));
          bool exists = false; for (auto &ip : slaveIPsDiscovered) if (ip == sip) exists = true;
          if (!exists) { slaveIPsDiscovered.push_back(sip); Serial.print("MASTER: discovered slave -> "); Serial.println(payload); }
        }
      }
    }
    doLedBlink();
    delay(200);
  }

  udp.stop();
  return !slaveIPsDiscovered.empty();
}

// SLAVE: počúva DISCOVER_MASTER a pošle unicast SLAVE_IP
bool slaveUdpListenAndReply(unsigned long timeoutMs) {
  if (!udp.begin(udp_port)) { Serial.println("UDP: failed to begin port"); return false; }

  unsigned long start = millis();
  while (!slaveDiscoveredByMaster && millis() - start < timeoutMs) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      int len = udp.read(buf, BUF_SZ - 1);
      if (len > 0) {
        buf[len] = 0;
        String payload = String(buf);
        if (payload.startsWith("DISCOVER_MASTER:")) {
          IPAddress myIP = ETH.localIP();
          String reply = String("SLAVE_IP:") + ipToStr(myIP);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write((const uint8_t*)reply.c_str(), reply.length());
          udp.endPacket();
          Serial.print("SLAVE: sent unicast reply -> "); Serial.println(reply);
          slaveDiscoveredByMaster = true;
        }
      }
    }
    doLedBlink();
    delay(50);
  }
  udp.stop();
  return slaveDiscoveredByMaster;
}

// ======================================================
// ===================== BOOT / SETUP ===================
// ======================================================
void runWifiSetupMode() {
  static bool started = false;
  if (!started) {
    Serial.println("\n=== ENTERING WIFI_SETUP MODE ===");

    shutdownLANForSetup();       // <<< sem

    startCaptivePortal();
    started = true;
  }

  dnsServer.processNextRequest();
  web.handleClient();

  if (!device_override && digitalRead(PIN_SETUP_SW) == HIGH) {
    stopCaptivePortal();
    delay(200);
    ESP.restart();
  }

  doLedBlink();
  delay(5);
}


void runBootMode() {
  if (!LAN_initialized) setupETH();

  if (IP_assign_for_all == "manual") {
    staticIP.fromString(my_IP);
    gwIP.fromString(localGateway);
    subnetIP.fromString(localSubnet);
    Serial.print("Manual IP config: ETH.config(" + my_IP + "," + localGateway + "," + localSubnet + ")");
    if (isIPValid(staticIP) && isIPValid(gwIP) && isIPValid(subnetIP)) {
      bool ok = ETH.config(staticIP, gwIP, subnetIP);
      if (!ok) Serial.println("BOOT: ETH.config returned false (maybe unsupported).");
    } else {
      Serial.println("BOOT: invalid manual IP/GW/SUBNET - check USER CONFIG.");
    }

    // naplň manuálne IP slávov (ak sú)
    if (slave_count >= 1 && slave1_IP.length() > 0) { IPAddress ip; ip.fromString(slave1_IP); if (isIPValid(ip)) slaveIPsManual.push_back(ip); }
    if (slave_count >= 2 && slave2_IP.length() > 0) { IPAddress ip; ip.fromString(slave2_IP); if (isIPValid(ip)) slaveIPsManual.push_back(ip); }
  } else {
    Serial.println("BOOT: waiting for DHCP-assigned IP...");
  }

  if (!LAN_initialized) {
    Serial.println("BOOT: initializing network...");

    if (IP_assign_for_all == "auto") {
      unsigned long start = millis();
      while (millis() - start < 10000) { // 10s
        if (ensureLinkAndIPReady()) { Serial.print("BOOT: working with IP: "); Serial.println(ipToStr(ETH.localIP())); break; }
        delay(200);
      }
      if (!isIPValid(ETH.localIP())) Serial.println("BOOT: warning - DHCP did not assign IP within 10s.");
    } else {
      delay(200);
      if (!ETH.linkUp()) {
        Serial.println("No Ethernet link – staying in BOOT mode...");
        while (!ETH.linkUp()) delay(500);
      }
    }

    if (!ensureIPAvailable()) {
      Serial.println("BOOT: no valid IP available - staying in BOOT.");
      LAN_initialized = false; delay(5000); return;
    }

    Serial.println("BOOT: network initialized.");
    LAN_initialized = true;
    enterMode(MODE_SEARCH);
  }
}

// ======================================================
// ======================= SEARCH =======================
// ======================================================
void runSearchMode() {
  Serial.println("=== SEARCH MODE ===");

  if (role == "master") {
    if (IP_assign_for_all == "auto") {
      Serial.println("SEARCH: UDP master runUdpDiscovery");
      bool ok = masterUdpDiscovery(5000);
      if (ok) Serial.println("SEARCH: UDP discovery done -> continuing with TCP handshake");
      else    Serial.println("SEARCH: UDP discovery failed or timeout.");
    }
    // (pri manual režime sa discovery preskakuje a ide sa rovno TCP)
  } else {
    // SLAVE: pred discovery/handshake čakaj na link a IP (po reštarte switche/DHCP to chvíľu trvá)
    if (!ensureLinkAndIPReady()) {
      Serial.println("SLAVE: no link/IP yet -> waiting...");
      delay(500);
      return;
    }
    if (IP_assign_for_all == "auto") {
      Serial.println("SEARCH: UDP slave listener...");
      slaveUdpListenAndReply(5000);
      if (!slaveDiscoveredByMaster) Serial.println("SLAVE: waiting for master UDP broadcast...");
    } else {
      slaveDiscoveredByMaster = true;
      Serial.println("SLAVE: manual IP mode -> skipping UDP discovery");
    }
  }

  // ---- TCP handshake ----
  Serial.println("=== SEARCH MODE (TCP handshake) ===");
  if (role == "master") {
    Serial.println("SEARCH (master): attempting TCP handshake to slaves...");

    std::vector<IPAddress> targets = (IP_assign_for_all == "auto") ? slaveIPsDiscovered : slaveIPsManual;
    if (targets.empty()) {
      Serial.println("SEARCH: No slave targets available (none discovered/configured). Staying in SEARCH.");
      delay(500);
      return;
    }

    peers.clear();
    for (auto &ip : targets) {
      String k = ipToStr(ip);
      Peer p; p.ip = ip; p.sockfd = -1; p.connected = false; p.lastHeartbeat = 0; p.confirmedWaitingForAll = false;
      peers[k] = p;
    }

    masterTcpConnectAll();

    unsigned long start = millis();
    const unsigned long handshakeTimeout = 5000;
    ledState = false; ledOff();  // počas handshake nech bliká vzor

    bool allConfirmed = false;
    while (millis() - start < handshakeTimeout) {
      masterHandleClients();
      sendHeartbeatsIfNeeded();
      dropIfTimedOut();
      doLedBlink();

      allConfirmed = true;
      for (auto &kv : peers) if (!kv.second.confirmedWaitingForAll) { allConfirmed = false; break; }
      if (allConfirmed) break;
      delay(10);
    }

    if (allConfirmed && !peers.empty()) {
      Serial.println("SEARCH: All slaves confirmed READY. Sending CMD:WORK_START ...");
      broadcastToPeers("CMD:WORK_START\n");
      Serial.println("MASTER: entering WORK mode...");
      enterMode(MODE_WORK);
    } else {
      Serial.println("SEARCH: Not all slaves confirmed. Staying in SEARCH mode.");
      for (auto &kv : peers) closePeer(kv.second);
      peers.clear();
    }

  } else {
    Serial.println("SEARCH (slave): starting TCP server to accept master connection...");
    startSlaveTcpServer();

    unsigned long start = millis();
    const unsigned long handshakeTimeout = 8000; // slave čaká dlhšie
    bool handshakeDone = false;

    while (millis() - start < handshakeTimeout) {
      handshakeDone = slaveTcpAcceptLoop();
      if (handshakeDone) break;
      doLedBlink();
      delay(10);
    }

    if (handshakeDone) { Serial.println("SLAVE: handshake completed. Entering WORK mode..."); enterMode(MODE_WORK); }
    else               { Serial.println("SLAVE: handshake timeout. Staying in SEARCH mode."); }
  }
}

// ======================================================
// ======================== WORK ========================
// ======================================================
void runWorkMode() {
  static bool relayOn   = false;
  static bool lastStart = false;
  static bool lastEstop = false;

  if (role == "master") {
    bool sPressed, eActive; instantDebounce(sPressed, eActive);


    // E-STOP hrana -> OFF + broadcast
    if (eActive && !lastEstop) {
      Serial.println("MASTER: E-STOP detected -> broadcasting CMD:EMERGENCY_STOP and RELAY OFF");
      broadcastToPeers("CMD:EMERGENCY_STOP\n");
      relayOn = false; setRelay(false);
      broadcastToPeers("CMD:RELAY:OFF\n");
    }

    // START hrana a nie je E-STOP
    if (sPressed && !eActive && !lastStart) {
      Serial.print("MASTER: START -> RELAY ON (to "); Serial.print(peers.size()); Serial.println(" peer(s))");
      relayOn = true; setRelay(true);
      broadcastToPeers("CMD:RELAY:ON\n");
    }

    // E-STOP držaný počas behu -> udrž OFF
    if (eActive && relayOn) {
      relayOn = false; setRelay(false);
      Serial.println("MASTER: ESTOP active -> enforce RELAY OFF");
      broadcastToPeers("CMD:RELAY:OFF\n");
    }

    // Heartbeat + dohľad spojenia
    sendHeartbeatsIfNeeded();
    masterHandleClients();
    bool lost = dropIfTimedOut();

    if (lost) { // okamžite failsafe + späť do SEARCH
      Serial.println("MASTER: link lost -> FAILSAFE OFF and re-discover");
      setRelay(false);
      broadcastToPeers("CMD:RELAY:OFF\n");
      enterMode(MODE_SEARCH);
      return;
    }

    bool anyLink = false;
    for (auto &kv : peers) { if (kv.second.connected) { anyLink = true; break; } }
    if (!anyLink) {
      Serial.println("MASTER: no peers connected -> re-discover");
      setRelay(false);
      enterMode(MODE_SEARCH);
      return;
    }

    lastStart = sPressed; lastEstop = eActive;
    delay(5);
  }
  else { // SLAVE
    // Spracuj prichádzajúce príkazy a HB
    for (auto &kv : peers) {
      Peer &p = kv.second; if (!p.connected) continue;

      int r = recv(p.sockfd, buf, BUF_SZ - 1, MSG_DONTWAIT);
      if (r > 0) {
        buf[r] = 0; p.rx += String(buf);
        int nl;
        while ((nl = p.rx.indexOf('\n')) >= 0) {
          String s = p.rx.substring(0, nl);
          p.rx.remove(0, nl + 1);
          if (s.length() == 0) continue;

          if      (s.startsWith("CMD:RELAY:ON"))  { relayOn = true;  setRelay(true);  Serial.println("SLAVE: RELAY ON"); }
          else if (s.startsWith("CMD:RELAY:OFF") || s.startsWith("CMD:EMERGENCY_STOP")) {
            relayOn = false; setRelay(false); Serial.println("SLAVE: RELAY OFF");
          }
          else if (s.startsWith("HB:MASTER")) {
            p.lastHeartbeat = millis();
            String resp = "HB:SLAVE\n";
            send(p.sockfd, resp.c_str(), resp.length(), 0);
            Serial.print("."); // heartbeat “keep-alive” bodka
          }
        }
      } else if (r == 0) {
        Serial.println("SLAVE: master closed connection");
        closePeer(p);
      }
    }

    // Pozatváraj mŕtve spojenia (timeouty)
    unsigned long now = millis();
    for (auto &kv : peers) {
      Peer &p = kv.second; if (!p.connected) continue;
      if (now - p.lastHeartbeat > COMMS_TIMEOUT) {
        Serial.print("SLAVE: HB timeout from "); Serial.println(ipToStr(p.ip));
        closePeer(p);
      }
    }

    // Link down/IP down -> OFF a späť do SEARCH
    if (!ensureLinkAndIPReady()) {
      if (relayOn) { relayOn = false; setRelay(false); Serial.println("SLAVE: link down -> RELAY OFF"); }
      closeAllPeers();
      enterMode(MODE_SEARCH);
      return;
    }

    // Nemáme nikoho pripojeného -> späť do SEARCH
    bool anyLink = false; for (auto &kv : peers) { if (kv.second.connected) { anyLink = true; break; } }
    if (!anyLink) {
      if (relayOn) { relayOn = false; setRelay(false); Serial.println("SLAVE: no peers -> RELAY OFF"); }
      enterMode(MODE_SEARCH);
      return;
    }

    delay(5);
  }
}

// ======================================================
// ==================== TCP HELPERS =====================
// ======================================================
void startSlaveTcpServer() {
  if (slave_server_running) return;

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { Serial.println("SLAVE TCP: failed to create listening socket."); return; }

  int opt = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in serv{}; serv.sin_family = AF_INET; serv.sin_port = htons(tcp_port); serv.sin_addr.s_addr = INADDR_ANY;

  if (bind(s, (struct sockaddr*)&serv, sizeof(serv)) < 0) { Serial.println("SLAVE TCP: bind failed."); close(s); return; }
  if (listen(s, 1) < 0) { Serial.println("SLAVE TCP: listen failed."); close(s); return; }

  makeNonBlocking(s);
  slave_listen_sock = s;
  slave_server_running = true;
  Serial.print("SLAVE TCP: listening on port "); Serial.println(tcp_port);
}

bool slaveTcpAcceptLoop() {
  if (!slave_server_running || slave_listen_sock < 0) return false;

  struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
  int client_sock = accept(slave_listen_sock, (struct sockaddr*)&cli, &clilen);
  if (client_sock >= 0) {
    makeNonBlocking(client_sock);
    IPAddress clientIP(cli.sin_addr.s_addr);
    String key = ipToStr(clientIP);
    Serial.print("SLAVE TCP: new connection from "); Serial.println(key);
    Peer p; p.ip = IPAddress(cli.sin_addr.s_addr); p.sockfd = client_sock; p.connected = true; p.lastHeartbeat = millis(); p.confirmedWaitingForAll = false;
    peers[key] = p;
  }

  bool handshakeDone = false;

  for (auto &kv : peers) {
    Peer &p = kv.second; if (!p.connected) continue;

    int r = recv(p.sockfd, buf, BUF_SZ - 1, MSG_DONTWAIT);
    if (r > 0) {
      buf[r] = 0; p.rx += String(buf);
      int nl;
      while ((nl = p.rx.indexOf('\n')) >= 0) {
        String s = p.rx.substring(0, nl);
        p.rx.remove(0, nl + 1);
        if (s.length() == 0) continue;

        if (s.startsWith("HANDSHAKE:MASTER_READY")) {
          String resp = "HANDSHAKE:SLAVE_READY\n";
          send(p.sockfd, resp.c_str(), resp.length(), 0);
          Serial.print(".");
          p.confirmedWaitingForAll = true;
          handshakeDone = true;
          slaveHandshakeDone = true;
          p.lastHeartbeat = millis();
        }
        else if (s.startsWith("HB:MASTER")) {
          p.lastHeartbeat = millis();
          String resp = "HB:SLAVE\n";
          send(p.sockfd, resp.c_str(), resp.length(), 0);
          Serial.print(".");
        }
        else if (s.startsWith("CMD:RELAY:ON")) {
          digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? LOW : HIGH);
        }
        else if (s.startsWith("CMD:RELAY:OFF") || s.startsWith("CMD:EMERGENCY_STOP")) {
          digitalWrite(PIN_RELAY, RELAY_ACTIVE_LOW ? HIGH : LOW);
        }
      }
    } else if (r == 0) {
      Serial.println("SLAVE TCP: client closed connection.");
      closePeer(p);
    }
  }

  return handshakeDone;
}

void masterTcpConnectAll() {
  for (auto &kv : peers) {
    Peer &p = kv.second;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { Serial.println("MASTER TCP: socket creation failed"); continue; }

    struct sockaddr_in serv{}; serv.sin_family = AF_INET; serv.sin_port = htons(tcp_port); serv.sin_addr.s_addr = (uint32_t)p.ip;

    Serial.print("MASTER TCP: attempting connect to "); Serial.println(ipToStr(p.ip));
    ledState = false; ledOff();
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) == 0) {
      p.sockfd = sock; p.connected = true; p.lastHeartbeat = millis();
      String msg = "HANDSHAKE:MASTER_READY\n";
      send(p.sockfd, msg.c_str(), msg.length(), 0);
      Serial.print("MASTER -> "); Serial.print(ipToStr(p.ip)); Serial.println(" : HANDSHAKE:MASTER_READY sent");
    } else {
      Serial.print("MASTER TCP: connect failed to "); Serial.println(ipToStr(p.ip));
      close(sock);
    }
  }
}

void masterHandleClients() {
  char buffer[128];
  for (auto &kv : peers) {
    Peer &p = kv.second; if (!p.connected) continue;

    int len = recv(p.sockfd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    if (len > 0) {
      buffer[len] = 0; p.rx += String(buffer);
      int nl;
      while ((nl = p.rx.indexOf('\n')) >= 0) {
        String s = p.rx.substring(0, nl);
        p.rx.remove(0, nl + 1);
        if (s.length() == 0) continue;

        if      (s.startsWith("HANDSHAKE:SLAVE_READY")) { p.confirmedWaitingForAll = true; p.lastHeartbeat = millis(); Serial.print("MASTER: slave "); Serial.print(ipToStr(p.ip)); Serial.println(" confirmed READY."); }
        else if (s.startsWith("HB:SLAVE"))              { p.lastHeartbeat = millis(); Serial.print("."); }
      }
    } else if (len == 0) {
      Serial.println("MASTER: slave closed connection");
      closePeer(p);
    }
  }
}

void sendHeartbeatsIfNeeded() {
  unsigned long now = millis();
  if (now - lastHeartbeatSend >= HEARTBEAT_INTERVAL) {
    lastHeartbeatSend = now;
    if (role == "master") {
      for (auto &kv : peers) {
        Peer &p = kv.second; if (!p.connected) continue;
        String msg = "HB:MASTER\n";
        send(p.sockfd, msg.c_str(), msg.length(), 0);
        Serial.print(".");
      }
    }
  }
}

bool dropIfTimedOut() {
  bool anyClosed = false;
  unsigned long now = millis();
  for (auto &kv : peers) {
    Peer &p = kv.second;
    if (!p.connected) continue;
    if (now - p.lastHeartbeat > COMMS_TIMEOUT) {
      Serial.print("COMMS timeout: "); Serial.println(ipToStr(p.ip));
      closePeer(p);
      anyClosed = true;
    }
  }
  return anyClosed;
}

void broadcastToPeers(const String &msg) {
  for (auto &kv : peers) {
    Peer &p = kv.second;
    if (p.connected) send(p.sockfd, msg.c_str(), msg.length(), 0);
  }
}

int makeNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0); if (flags == -1) flags = 0;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void closePeer(Peer &p) {
  if (p.sockfd >= 0) { close(p.sockfd); p.sockfd = -1; }
  p.connected = false; p.confirmedWaitingForAll = false;
}

void closeAllPeers() {
  for (auto &kv : peers) closePeer(kv.second);
}

// ======================================================
// ================== STRING UTILITIES ==================
// ======================================================
String ipToStr(IPAddress ip) {
  return String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
}
// ======================================================
// ================== SD CARD utility  ==================
// ======================================================
// Mountni SD_MMC (1-bit mód) – volá sa len raz, ak treba
static bool ensureSDMounted() {
  if (g_sd_mounted) return true;

  // "/sdcard", true = 1-bit mód na ESP32
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD_MMC: begin() FAILED");
    g_sd_mounted = false;
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("SD_MMC: no card");
    g_sd_mounted = false;
    return false;
  }

  g_sd_mounted = true;
  Serial.println("SD_MMC: mounted OK (1-bit)");
  return true;
}

// Voľné miesto v KB (ak vie knižnica zistiť)
static int64_t sdFreeKB() {
#if defined(ESP32)
  uint64_t tot = SD_MMC.totalBytes();
  uint64_t usd = SD_MMC.usedBytes();
  if (tot == 0 || usd > tot) return -1;
  return (int64_t)((tot - usd) / 1024ULL);
#else
  return -1;
#endif
}

// Nájde ďalší voľný názov 00000001.txt .. 99999999.txt v root-e
static bool pickNextLogFile(String &outName) {
  for (uint32_t i = 1; i <= 99999999UL; ++i) {
    char name[20];
    snprintf(name, sizeof(name), "/%08lu.txt", (unsigned long)i);
    if (!SD_MMC.exists(name)) { outName = String(name); return true; }
  }
  return false;
}
// All-in-one zápis: first_write_after_boot vyberie nový súbor, inak append
bool write_SD(bool first_write_after_boot, const String& text) {
  // 0) mount
  if (!ensureSDMounted()) {
    Serial.println("SD_MMC: not available");
    return false;
  }

  // 1) voľné miesto (ak vieme)
  int64_t freeKB = sdFreeKB();
  if (freeKB >= 0 && (uint32_t)freeKB < SD_MIN_FREE_KB) {
    Serial.print("SD_MMC: low space (KB="); Serial.print((long)freeKB); Serial.println(")");
    return false;
  }

  // 2) prvé volanie po boote -> vyber nový názov
  if (first_write_after_boot || g_sd_logfile.length() == 0) {
    String newName;
    if (!pickNextLogFile(newName)) {
      Serial.println("SD_MMC: no free filename (00000001.txt..)");
      return false;
    }
    // vytvor súbor hneď
    File f = SD_MMC.open(newName, FILE_WRITE);
    if (!f) {
      Serial.print("SD_MMC: create fail "); Serial.println(newName);
      return false;
    }
    f.close();
    g_sd_logfile = newName;
    Serial.print("SD_MMC: logfile = "); Serial.println(g_sd_logfile);
  }

  // 3) append riadku: "time>>" + text
  File f = SD_MMC.open(g_sd_logfile, FILE_APPEND);
  if (!f) {
    Serial.print("SD_MMC: open append fail "); Serial.println(g_sd_logfile);
    return false;
  }

  String line = "time>>" + text + "\n"; // time je placeholder
  size_t wr = f.print(line);
  f.flush();
  f.close();

  if (wr != line.length()) {
    Serial.println("SD_MMC: partial/failed write");
    return false;
  }
  return true;
}