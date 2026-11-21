#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

namespace {
constexpr char kFirmwareName[] = "Mosaico S3";
constexpr char kFirmwareVersion[] = "0.1.0";
constexpr char kApPassword[] = "mosaico123";

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;
bool provisioningMode = false;
String ssid;
String password;

String escapeJson(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

String systemName() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char name[24];
    snprintf(name, sizeof(name), "Mosaico-%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(name);
}

void sendIndex() {
    static const char indexHtml[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Mosaico</title>
  <style>
    :root{color-scheme:dark;--bg:#090d18;--panel:#141b2d;--accent:#6ef0c2;--text:#edf4ff}
    *{box-sizing:border-box}body{margin:0;font-family:Inter,system-ui,sans-serif;background:radial-gradient(circle at 20% 10%,#1b3150,#090d18 45%);color:var(--text)}
    main{max-width:760px;margin:auto;padding:24px}.brand{display:flex;align-items:center;gap:14px;margin:28px 0}
    .mark{width:54px;height:54px;border-radius:16px;background:linear-gradient(135deg,#70e1c2,#6c8cff);box-shadow:0 12px 35px #50dcb044}
    h1{font-size:28px;margin:0}.version{color:#91a0b8;font-size:13px}
    .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.card{background:#141b2dcc;border:1px solid #ffffff12;border-radius:18px;padding:16px}
    .card h2{font-size:13px;color:#91a0b8;text-transform:uppercase;letter-spacing:.12em;margin:0 0 12px}.value{font-size:18px;font-weight:700}
    .wide{grid-column:1/-1}.row{display:flex;gap:8px;margin-top:10px}input{width:100%;padding:12px;border-radius:10px;border:1px solid #ffffff20;background:#0c1220;color:white}
    button{border:0;border-radius:10px;padding:11px 16px;background:var(--accent);color:#07120f;font-weight:800;cursor:pointer}.danger{background:#ff7189;color:#25060b}
    .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#6ef0c2;margin-right:7px}.progress{display:none;height:8px;border-radius:8px;background:#0a0f1b;overflow:hidden;margin-top:12px}.progress i{display:block;height:100%;width:0;background:var(--accent)}
  </style>
</head>
<body><main>
  <header class="brand"><div class="mark"></div><div><h1>Mosaico</h1><div class="version" id="firmware">Starting...</div></div></header>
  <section class="grid">
    <div class="card"><h2>Device</h2><div class="value" id="device">--</div></div>
    <div class="card"><h2>Network</h2><div class="value" id="network">--</div></div>
    <div class="card wide"><h2>Wi-Fi setup</h2>
      <input id="ssid" maxlength="32" placeholder="Network name">
      <div class="row"><input id="password" maxlength="64" type="password" placeholder="Password"><button onclick="saveWifi()">Connect</button></div>
    </div>
    <div class="card wide"><h2>Firmware update</h2>
      <input id="firmwareFile" type="file" accept=".bin">
      <div class="row"><button onclick="uploadFirmware()">Upload firmware</button><button class="danger" onclick="restart()">Restart</button></div>
      <div class="progress" id="progress"><i id="progressBar"></i></div>
    </div>
  </section>
</main>
<script>
async function refresh(){
  const s=await (await fetch('/api/status')).json();
  document.querySelector('#firmware').textContent=s.name+' '+s.version;
  document.querySelector('#device').textContent=s.chip+' · '+(s.psramMB>0?s.psramMB+' MB PSRAM':'No PSRAM');
  document.querySelector('#network').innerHTML='<span class="dot"></span>'+s.wifi;
}
async function saveWifi(){
  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid.value,password:password.value})});
  alert((await r.json()).message);
}
async function restart(){await fetch('/api/restart',{method:'POST'});setTimeout(refresh,1500)}
async function uploadFirmware(){
  const file=document.querySelector('#firmwareFile').files[0];if(!file)return alert('Choose a firmware .bin file');
  const bar=document.querySelector('#progressBar'),wrap=document.querySelector('#progress');wrap.style.display='block';
  const req=new XMLHttpRequest();req.open('POST','/api/update');
  req.upload.onprogress=e=>bar.style.width=(e.loaded/e.total*100).toFixed(0)+'%';
  req.onload=()=>alert(req.status===200?'Update complete. Device will restart.':req.responseText);
  req.send(file);
}
refresh();setInterval(refresh,5000);
</script></body></html>)rawliteral";
    server.send_P(200, "text/html", indexHtml);
}

void sendStatus() {
    esp_chip_info_t chipInfo;
    esp_chip_info(&chipInfo);
    char chip[48];
    snprintf(chip, sizeof(chip), "ESP32-S3 rev %d", chipInfo.revision);

    String network = WiFi.status() == WL_CONNECTED
        ? WiFi.SSID() + " · " + WiFi.localIP().toString()
        : (provisioningMode ? WiFi.softAPSSID() + " · Provisioning" : "Not connected");

    String json = "{";
    json += "\"name\":\"" + escapeJson(kFirmwareName) + "\",";
    json += "\"version\":\"" + escapeJson(kFirmwareVersion) + "\",";
    json += "\"chip\":\"" + escapeJson(chip) + "\",";
    json += "\"psramMB\":" + String(ESP.getPsramSize() / 1048576) + ",";
    json += "\"wifi\":\"" + escapeJson(network) + "\",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"uptime\":" + String(millis() / 1000);
    json += "}";
    server.send(200, "application/json", json);
}

void startAccessPoint() {
    provisioningMode = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(systemName().c_str(), kApPassword);
    WiFi.setHostname(systemName().c_str());
    dnsServer.start(53, "*", WiFi.softAPIP());
    Serial.printf("Provisioning AP: %s  Password: %s  IP: %s\n",
                  systemName().c_str(), kApPassword, WiFi.softAPIP().toString().c_str());
}

bool connectSavedWifi() {
    preferences.begin("mosaico", true);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    preferences.end();

    if (ssid.isEmpty()) {
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(systemName().c_str());
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.printf("Connecting to %s", ssid.c_str());
    const uint32_t timeout = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < timeout) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();
    return WiFi.status() == WL_CONNECTED;
}

void configureRoutes() {
    server.on("/", HTTP_GET, sendIndex);
    server.on("/api/status", HTTP_GET, sendStatus);

    server.on("/api/wifi", HTTP_POST, []() {
        if (!server.hasArg("plain")) {
            server.send(400, "application/json", "{\"message\":\"Invalid request\"}");
            return;
        }
        String body = server.arg("plain");
        auto stringArg = [&body](const char* key) {
            String needle = String("\"") + key + "\":\"";
            int start = body.indexOf(needle);
            if (start < 0) return String("");
            start += needle.length();
            int end = body.indexOf('"', start);
            if (end < 0) return String("");
            return body.substring(start, end);
        };

        String newSsid = stringArg("ssid");
        String newPassword = stringArg("password");
        if (newSsid.isEmpty()) {
            server.send(400, "application/json", "{\"message\":\"Network name is required\"}");
            return;
        }
        preferences.begin("mosaico", false);
        preferences.putString("ssid", newSsid);
        preferences.putString("password", newPassword);
        preferences.end();
        server.send(200, "application/json", "{\"message\":\"Credentials saved. Restarting...\"}");
        delay(300);
        ESP.restart();
    });

    server.on("/api/restart", HTTP_POST, []() {
        server.send(200, "application/json", "{\"message\":\"Restarting\"}");
        delay(300);
        ESP.restart();
    });

    server.on("/api/update", HTTP_POST, []() {
        if (Update.hasError()) {
            server.send(500, "text/plain", "OTA update failed");
        } else {
            server.send(200, "text/plain", "Update complete");
            delay(300);
            ESP.restart();
        }
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            Serial.printf("OTA start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Serial.println(Update.errorString());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Serial.println(Update.errorString());
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) {
                Serial.printf("OTA success: %u bytes\n", upload.totalSize);
            } else {
                Serial.println(Update.errorString());
            }
        }
    });

    server.onNotFound([]() {
        if (provisioningMode) {
            sendIndex();
        } else {
            server.send(404, "text/plain", "Not found");
        }
    });
}
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\nMosaico S3 boot");
    Serial.printf("Firmware: %s %s\n", kFirmwareName, kFirmwareVersion);
    Serial.printf("Flash: %u MB  PSRAM: %u MB  Heap: %u bytes\n",
                  ESP.getFlashChipSize() / 1048576,
                  ESP.getPsramSize() / 1048576,
                  ESP.getFreeHeap());

    if (!connectSavedWifi()) {
        startAccessPoint();
    } else {
        provisioningMode = false;
        Serial.printf("Connected to %s  IP: %s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
    }

    configureRoutes();
    server.begin();
}

void loop() {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(2);
}
