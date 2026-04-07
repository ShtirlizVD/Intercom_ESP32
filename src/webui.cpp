/*
 * webui.cpp - Реализация веб-интерфейса настройки
 *
 * HTTP API + captive portal для начальной настройки WiFi.
 */

#include "webui.h"
#include "config.h"
#include "intercom.h"
#include "pins.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <LITTLEFS.h>

// Статические члены
WebServer* WebUI::server = nullptr;
DNSServer* WebUI::dnsServer = nullptr;
bool WebUI::apMode = false;
uint32_t WebUI::apStartTime = 0;

// ==================== HTML страница (встроенная в прошивку) ====================
// Встроенный HTML используется как fallback если файл в LITTLEFS не найден
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>ESP32 Intercom</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#0f1923;color:#e0e0e0;padding:16px;min-height:100vh}
.container{max-width:480px;margin:0 auto}
h1{text-align:center;color:#00d4ff;margin-bottom:16px;font-size:1.5em}
.subtitle{text-align:center;color:#666;margin-bottom:20px;font-size:0.85em}
.status{text-align:center;padding:12px;border-radius:8px;margin-bottom:16px;font-weight:600}
.status.ok{background:#1b4332;color:#52b788}
.status.err{background:#3d0c02;color:#e63946}
.status.call{background:#4a1942;color:#e056a0}
.status.ring{background:#433402;color:#e0c020}
.card{background:#16213e;border-radius:10px;padding:16px;margin-bottom:12px;border:1px solid #1a3050}
.card h2{color:#00d4ff;margin-bottom:12px;font-size:1.1em}
label{display:block;margin-bottom:4px;color:#8899aa;font-size:0.85em;margin-top:8px}
input,select{width:100%;padding:10px;border:1px solid #2a3a4a;border-radius:6px;background:#0d1520;color:#e0e0e0;font-size:0.95em;-webkit-appearance:none}
input:focus,select:focus{outline:none;border-color:#00d4ff}
button{background:#00d4ff;color:#000;border:none;padding:12px 16px;border-radius:6px;cursor:pointer;width:100%;font-size:0.95em;font-weight:600;margin-top:8px;transition:background 0.2s}
button:hover{background:#00a8cc}
button:active{transform:scale(0.98)}
.btn-row{display:flex;gap:8px;margin-top:8px}
.btn-row button{flex:1}
.btn-danger{background:#e63946!important}
.btn-danger:hover{background:#c1121f!important}
.btn-ok{background:#2d6a4f!important}
.btn-ok:hover{background:#1b4332!important;color:#fff!important}
.btn-warn{background:#e07b00!important;color:#000!important}
.btn-warn:hover{background:#c06600!important}
#wifi-list{max-height:200px;overflow-y:auto;margin:8px 0}
.net{padding:10px;border:1px solid #2a3a4a;border-radius:6px;margin-bottom:4px;cursor:pointer;transition:background 0.15s;display:flex;justify-content:space-between;align-items:center}
.net:hover{background:#1a2a3a}
.net .ssid{font-weight:600;font-size:0.9em}
.net .info{color:#666;font-size:0.75em}
.net .lock{color:#e07b00;margin-left:6px}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;font-size:0.85em}
.info-grid span{color:#666}
.info-grid strong{color:#a0c0e0}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#333;color:#fff;padding:12px 24px;border-radius:8px;opacity:0;transition:opacity 0.3s;z-index:100;font-size:0.9em}
.toast.show{opacity:1}
.range-val{color:#00d4ff;font-weight:600}
</style>
</head>
<body>
<div class="container">
<h1>📞 ESP32 Intercom</h1>
<div class="subtitle">Полнодуплексный интерком по Wi-Fi</div>
<div id="status" class="status err">Инициализация...</div>

<div class="card">
<h2>📡 Wi-Fi подключение</h2>
<button onclick="scanWiFi()">🔍 Найти сети</button>
<div id="wifi-list"></div>
<label>SSID (имя сети)</label>
<input type="text" id="ssid" placeholder="Введите или выберите выше">
<label>Пароль</label>
<input type="password" id="pass" placeholder="Пароль Wi-Fi">
<div class="btn-row">
<button class="btn-ok" onclick="connectWiFi()">💾 Подключить</button>
<button class="btn-warn" onclick="disconnectWiFi()">Отключить</button>
</div>
</div>

<div class="card">
<h2>⚙️ Устройство</h2>
<label>Имя устройства</label>
<input type="text" id="devname" placeholder="ESP32-Intercom">
<label>Режим кнопки</label>
<select id="btnmode">
<option value="0">📞 Телефон (полнодуплекс)</option>
<option value="1">🎙️ Push-to-Talk</option>
</select>
</div>

<div class="card">
<h2>📞 Интерком</h2>
<label>IP удалённого устройства</label>
<input type="text" id="remoteip" placeholder="192.168.1.100">
<div class="btn-row">
<div style="flex:1">
<label>Порт управления</label>
<input type="number" id="ctrlport" value="8080">
</div>
<div style="flex:1">
<label>Порт аудио</label>
<input type="number" id="audioport" value="8081">
</div>
</div>
</div>

<div class="card">
<h2>🔊 Аудио</h2>
<label>Частота дискретизации</label>
<select id="samplerate">
<option value="8000">8000 Hz (экономия)</option>
<option value="16000" selected>16000 Hz (рекомендуется)</option>
<option value="22050">22050 Hz (повышенное)</option>
<option value="44100">44100 Hz (высокое качество)</option>
</select>
<label>Усиление микрофона: <span class="range-val" id="mgain-val">5</span>/10</label>
<input type="range" id="mgain" min="0" max="10" value="5" oninput="document.getElementById('mgain-val').textContent=this.value">
<label>Громкость динамика: <span class="range-val" id="svol-val">7</span>/10</label>
<input type="range" id="svol" min="0" max="10" value="7" oninput="document.getElementById('svol-val').textContent=this.value">
</div>

<div class="card">
<h2>🔧 Управление</h2>
<button class="btn-ok" onclick="saveConfig()">💾 Сохранить настройки</button>
<div class="btn-row">
<button class="btn-danger" onclick="reboot()">🔁 Перезагрузить ESP32</button>
<button class="btn-danger" onclick="factoryReset()">⚠️ Сброс настроек</button>
</div>
</div>

<div class="card">
<h2>ℹ️ Информация об устройстве</h2>
<div class="info-grid" id="info"></div>
</div>
</div>

<div class="toast" id="toast"></div>

<script>
var lastStatus='';

function toast(msg){
  var t=document.getElementById('toast');
  t.textContent=msg;t.classList.add('show');
  setTimeout(function(){t.classList.remove('show')},3000);
}

function api(method,path,body,cb){
  var x=new XMLHttpRequest();
  x.open(method,path,true);
  x.setRequestHeader('Content-Type','application/json');
  x.onload=function(){
    try{cb(JSON.parse(x.responseText));}
    catch(e){cb({error:x.responseText});}
  };
  x.onerror=function(){cb({error:'Сетевая ошибка'});};
  if(body)x.send(JSON.stringify(body));else x.send();
}

function loadConfig(){
  api('GET','/api/config',{},function(d){
    if(d.error){toast('Ошибка: '+d.error);return;}
    document.getElementById('ssid').value=d.wifi_ssid||'';
    document.getElementById('devname').value=d.device_name||'';
    document.getElementById('btnmode').value=d.button_mode||'0';
    document.getElementById('remoteip').value=d.remote_ip||'';
    document.getElementById('ctrlport').value=d.ctrl_port||8080;
    document.getElementById('audioport').value=d.audio_port||8081;
    document.getElementById('samplerate').value=d.sample_rate||16000;
    document.getElementById('mgain').value=d.mic_gain||5;
    document.getElementById('mgain-val').textContent=d.mic_gain||5;
    document.getElementById('svol').value=d.spk_volume||7;
    document.getElementById('svol-val').textContent=d.spk_volume||7;
  });
}

function updateStatus(){
  api('GET','/api/status',{},function(d){
    var el=document.getElementById('status');
    if(d.wifi_connected){
      el.className='status ok';
      el.textContent='✅ Wi-Fi: '+d.wifi_ssid+' | IP: '+d.ip;
    }else if(d.ap_mode){
      el.className='status err';
      el.textContent='🔴 Режим точки доступа: '+d.ap_ssid;
    }else{
      el.className='status err';
      el.textContent='❌ Не подключён к Wi-Fi';
    }
    // Call state
    var cs=d.call_state||'IDLE';
    if(cs==='IN_CALL'&&lastStatus!=='IN_CALL'){
      el.className='status call';
      el.textContent='🟢 Разговор ('+d.call_duration_sec+' сек)';
    }else if(cs==='RINGING_IN'&&lastStatus!=='RINGING_IN'){
      el.className='status ring';
      el.textContent='📞 Входящий вызов от '+d.caller;
    }else if(cs==='RINGING_OUT'){
      el.className='status ring';
      el.textContent='📞 Вызов...';
    }
    lastStatus=cs;

    // Info panel
    document.getElementById('info').innerHTML=
      '<span>MAC:</span><strong>'+(d.mac||'-')+'</strong>'+
      '<span>IP:</span><strong>'+(d.ip||'-')+'</strong>'+
      '<span>Шлюз:</span><strong>'+(d.gateway||'-')+'</strong>'+
      '<span>RSSI:</span><strong>'+(d.rssi||'-')+' dBm</strong>'+
      '<span>Вызов:</span><strong>'+(d.call_state_name||'Ожидание')+'</strong>'+
      '<span>Свободно:</span><strong>'+(d.free_heap||'-')+' KB</strong>';
  });
}

function scanWiFi(){
  document.getElementById('wifi-list').innerHTML='<p>Сканирование...</p>';
  api('POST','/api/wifi/scan',{},function(d){
    if(d.error){toast('Ошибка сканирования');return;}
    var el=document.getElementById('wifi-list');
    if(!d.networks||d.networks.length===0){
      el.innerHTML='<p>Сети не найдены</p>';return;
    }
    el.innerHTML=d.networks.map(function(n){
      var lock=n.secured?'<span class="lock">🔒</span>':'';
      return '<div class="net" onclick="document.getElementById(\'ssid\').value=\''+n.ssid.replace(/'/g,"\\'")+'\'">'+
        '<span class="ssid">'+n.ssid+lock+'</span>'+
        '<span class="info">'+n.rssi+' dBm | Ch.'+n.channel+'</span></div>';
    }).join('');
  });
}

function connectWiFi(){
  var s=document.getElementById('ssid').value;
  var p=document.getElementById('pass').value;
  if(!s){toast('Введите SSID!');return;}
  toast('Подключение к '+s+'...');
  api('POST','/api/wifi/connect',{ssid:s,password:p},function(d){
    if(d.ok)toast('Подключено! IP: '+d.ip);
    else toast('Ошибка: '+(d.error||''));
    setTimeout(updateStatus,3000);
  });
}

function disconnectWiFi(){
  api('POST','/api/wifi/disconnect',{},function(d){
    toast('Отключено от Wi-Fi');
    setTimeout(updateStatus,1000);
  });
}

function saveConfig(){
  var c={
    device_name:document.getElementById('devname').value,
    button_mode:parseInt(document.getElementById('btnmode').value),
    remote_ip:document.getElementById('remoteip').value,
    ctrl_port:parseInt(document.getElementById('ctrlport').value),
    audio_port:parseInt(document.getElementById('audioport').value),
    sample_rate:parseInt(document.getElementById('samplerate').value),
    mic_gain:parseInt(document.getElementById('mgain').value),
    spk_volume:parseInt(document.getElementById('svol').value)
  };
  api('POST','/api/config',c,function(d){
    if(d.ok)toast('Настройки сохранены!');
    else toast('Ошибка: '+(d.error||''));
  });
}

function reboot(){
  if(!confirm('Перезагрузить ESP32?'))return;
  api('POST','/api/reboot',{},function(){toast('Перезагрузка...');});
}

function factoryReset(){
  if(!confirm('Сбросить ВСЕ настройки? Устройство перезагрузится.'))return;
  api('POST','/api/factory-reset',{},function(){toast('Сброс... перезагрузка');});
}

loadConfig();
updateStatus();
setInterval(updateStatus,3000);
</script>
</body>
</html>
)rawliteral";

// ==================== Инициализация ====================

void WebUI::init() {
    if (server != nullptr) {
        delete server;
    }
    server = new WebServer(WEB_PORT);

    // Роуты
    server->on("/", HTTP_GET, handleRoot);
    server->on("/index.html", HTTP_GET, handleIndexHTML);

    // API
    server->on("/api/status", HTTP_GET, handleAPIStatus);
    server->on("/api/config", HTTP_GET, handleAPIGetConfig);
    server->on("/api/config", HTTP_POST, handleAPISetConfig);
    server->on("/api/wifi/scan", HTTP_POST, handleAPIWiFiScan);
    server->on("/api/wifi/connect", HTTP_POST, handleAPIWiFiConnect);
    server->on("/api/wifi/disconnect", HTTP_POST, [](WebServer& s) {
        WiFi.disconnect(true);
        WebUI::sendJSON(200, "{\"ok\":true}");
    });
    server->on("/api/reboot", HTTP_POST, handleAPIReboot);
    server->on("/api/factory-reset", HTTP_POST, handleAPIFactoryReset);

    // Captive portal — перенаправление на /
    server->onNotFound(handleNotFound);

    server->begin();
    Serial.printf("[WEBUI] HTTP сервер запущен на порту %d\n", WEB_PORT);
}

void WebUI::handleClient() {
    if (server) {
        server->handleClient();
    }
    if (dnsServer && apMode) {
        dnsServer->processNextRequest();
    }
}

void WebUI::startAP() {
    DeviceConfig& cfg = Config::get();

    char apSSID[64];
    snprintf(apSSID, sizeof(apSSID), "%s_%s", AP_SSID_PREFIX, cfg.device_name);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(apSSID, AP_PASSWORD);
    delay(100);

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[WEBUI] AP запущен: %s (IP: %s)\n", apSSID, apIP.toString().c_str());

    // DNS сервер для captive portal (перенаправляет все запросы на ESP32)
    if (dnsServer == nullptr) {
        dnsServer = new DNSServer();
    }
    dnsServer->start(53, "*", apIP);

    apMode = true;
    apStartTime = millis();

    init();
}

bool WebUI::startSTA() {
    DeviceConfig& cfg = Config::get();

    if (!Config::hasWiFi()) {
        Serial.println("[WEBUI] WiFi не настроен, запускаю AP");
        startAP();
        return false;
    }

    Serial.printf("[WEBUI] Подключение к WiFi: %s\n", cfg.wifi_ssid);

    WiFi.mode(WIFI_STA);

    // Статический IP если настроен
    if (!cfg.dhcp_enabled && strlen(cfg.static_ip) > 0) {
        IPAddress ip, gw, mask;
        ip.fromString(cfg.static_ip);
        gw.fromString(cfg.static_gw);
        mask.fromString(cfg.static_netmask);
        WiFi.config(ip, gw, mask);
    }

    WiFi.begin(cfg.wifi_ssid, cfg.wifi_password);

    // Ожидание подключения (10 секунд)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        apMode = false;
        Serial.printf("[WEBUI] WiFi подключён! IP: %s (RSSI: %d dBm)\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());

        // Если имя по умолчанию — обновить
        Config::setDefaultName();

        init();
        return true;
    }

    Serial.println("[WEBUI] Не удалось подключиться к WiFi, запускаю AP");
    startAP();
    return false;
}

bool WebUI::isAPMode() {
    return apMode;
}

// ==================== Обработчики ====================

void WebUI::handleRoot() {
    // Перенаправление на index.html
    server->sendHeader("Location", "/index.html");
    server->send(302);
}

void WebUI::handleIndexHTML() {
    // Пытаемся загрузить из LittleFS
    if (LITTLEFS.exists("/index.html")) {
        File f = LITTLEFS.open("/index.html", "r");
        server->streamFile(f, "text/html");
        f.close();
    } else {
        // Fallback: встроенная HTML страница
        server->send_P(200, "text/html", INDEX_HTML);
    }
}

void WebUI::handleAPIStatus() {
    JsonDocument doc;
    doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["ap_mode"] = apMode;
    doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["gateway"] = apMode ? "0.0.0.0" : WiFi.gatewayIP().toString();
    doc["subnet"] = apMode ? "255.255.255.0" : WiFi.subnetMask().toString();
    doc["free_heap"] = ESP.getFreeHeap() / 1024;
    doc["uptime"] = millis() / 1000;

    // WiFi info
    if (apMode) {
        doc["ap_ssid"] = String(AP_SSID_PREFIX) + "_" + Config::get().device_name;
        doc["wifi_ssid"] = "";
    } else {
        doc["ap_ssid"] = "";
        doc["wifi_ssid"] = WiFi.SSID();
    }

    // Call state
    doc["call_state"] = (int)Intercom::getState();
    doc["call_state_name"] = Intercom::getStateName();
    doc["call_duration_sec"] = Intercom::getCallDuration() / 1000;

    String output;
    serializeJson(doc, output);
    server->send(200, "application/json", output);
}

void WebUI::handleAPIGetConfig() {
    DeviceConfig& cfg = Config::get();
    JsonDocument doc;

    doc["wifi_ssid"] = String(cfg.wifi_ssid);
    doc["wifi_password"] = "********";  // Не отправляем пароль
    doc["dhcp_enabled"] = cfg.dhcp_enabled;
    doc["static_ip"] = String(cfg.static_ip);
    doc["static_gw"] = String(cfg.static_gw);
    doc["static_netmask"] = String(cfg.static_netmask);
    doc["device_name"] = String(cfg.device_name);
    doc["button_mode"] = (int)cfg.button_mode;
    doc["remote_ip"] = String(cfg.remote_ip);
    doc["ctrl_port"] = cfg.ctrl_port;
    doc["audio_port"] = cfg.audio_port;
    doc["sample_rate"] = cfg.sample_rate;
    doc["mic_gain"] = cfg.mic_gain;
    doc["spk_volume"] = cfg.spk_volume;

    String output;
    serializeJson(doc, output);
    server->send(200, "application/json", output);
}

void WebUI::handleAPISetConfig() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server->arg("plain"));

    if (err) {
        sendJSON(400, "{\"error\":\"Invalid JSON\"}");
        return;
    }

    DeviceConfig& cfg = Config::get();

    // Устройство
    if (doc.containsKey("device_name")) {
        String name = doc["device_name"].as<String>();
        strncpy(cfg.device_name, name.c_str(), MAX_NAME_LEN);
    }

    // Режим кнопки
    if (doc.containsKey("button_mode")) {
        cfg.button_mode = (ButtonMode)doc["button_mode"].as<int>();
    }

    // Интерком
    if (doc.containsKey("remote_ip")) {
        String rip = doc["remote_ip"].as<String>();
        strncpy(cfg.remote_ip, rip.c_str(), MAX_IP_LEN);
        cfg.remote_configured = (strlen(cfg.remote_ip) > 0);
    }
    if (doc.containsKey("ctrl_port")) {
        cfg.ctrl_port = doc["ctrl_port"].as<uint16_t>();
    }
    if (doc.containsKey("audio_port")) {
        cfg.audio_port = doc["audio_port"].as<uint16_t>();
    }

    // Аудио
    if (doc.containsKey("sample_rate")) {
        cfg.sample_rate = doc["sample_rate"].as<uint32_t>();
    }
    if (doc.containsKey("mic_gain")) {
        cfg.mic_gain = doc["mic_gain"].as<uint8_t>();
    }
    if (doc.containsKey("spk_volume")) {
        cfg.spk_volume = doc["spk_volume"].as<uint8_t>();
    }

    Config::save();

    // Если нужно — переконфигурируем I2S
    // (Для простоты — просто сохраняем, изменение SR требует перезагрузки)

    sendJSON(200, "{\"ok\":true}");
}

void WebUI::handleAPIWiFiScan() {
    JsonDocument doc;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (apMode) {
        // В AP режиме сканируем STA интерфейс
        WiFi.scanNetworks(true);  // Асинхронное сканирование
        delay(2000);  // Ждём результаты
    } else {
        WiFi.scanNetworks(true);
        delay(2000);
    }

    int n = WiFi.scanComplete();
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            JsonObject net = networks.add<JsonObject>();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            net["channel"] = WiFi.channel(i);
            net["secured"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
    }

    WiFi.scanDelete();

    String output;
    serializeJson(doc, output);
    server->send(200, "application/json", output);
}

void WebUI::handleAPIWiFiConnect() {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, server->arg("plain"));

    if (err || !doc.containsKey("ssid")) {
        sendJSON(400, "{\"error\":\"SSID required\"}");
        return;
    }

    String ssid = doc["ssid"].as<String>();
    String password = doc.containsKey("password") ? doc["password"].as<String>() : "";

    Config::setWiFi(ssid.c_str(), password.c_str());

    sendJSON(200, "{\"ok\":true,\"message\":\"Saved. Rebooting...\"}");

    // Перезагружаемся для применения WiFi настроек
    delay(500);
    ESP.restart();
}

void WebUI::handleAPIReboot() {
    sendJSON(200, "{\"ok\":true,\"message\":\"Rebooting...\"}");
    delay(500);
    ESP.restart();
}

void WebUI::handleAPIFactoryReset() {
    Config::reset();
    sendJSON(200, "{\"ok\":true,\"message\":\"Factory reset. Rebooting...\"}");
    delay(500);
    ESP.restart();
}

void WebUI::handleNotFound() {
    // Captive portal detection
    String host = server->hostHeader();

    // Перенаправляем любые запросы на главную страницу
    if (apMode) {
        // Для captive portal: если запрошен не наш IP — перенаправляем
        server->sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
        server->send(302);
    } else {
        server->send(404, "text/plain", "Not Found");
    }
}

void WebUI::sendJSON(int code, const char* json) {
    server->sendHeader("Access-Control-Allow-Origin", "*");
    server->sendHeader("Content-Type", "application/json");
    server->send(code, "application/json", json);
}
