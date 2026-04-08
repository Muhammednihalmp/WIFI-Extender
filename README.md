# 📡 ESP8266 WiFi Repeater / Extender — Butler GUI

> **Live Demo →** [https://muhammednihalmp.github.io/WIFI-Extender/](https://muhammednihalmp.github.io/WIFI-Extender/)

A full-featured WiFi repeater/extender firmware for the **ESP8266** with a powerful built-in web control panel called **Butler GUI**. Connect it to your existing router and instantly extend your WiFi coverage — no extra hardware or apps needed.

---

## ✨ Features

| Feature | Details |
|---|---|
| 🔁 NAT Repeater | Full lwIP hardware NAT — AP clients get real internet |
| 🌐 Butler Web GUI | Single-page control panel at `192.168.4.1` |
| 💾 Persistent Config | All settings saved to LittleFS (survives reboot) |
| 📡 AP + STA mode | Simultaneous upstream client + access point |
| 🔍 Network Scanner | Scan nearby SSIDs from the GUI |
| 👥 Client List | See connected device MACs in real time |
| 📶 TX Power Control | Adjustable output power (0–20.5 dBm) |
| 🐕 Watchdog | Auto-reconnect + hard reboot after 5 min offline |
| 🔄 OTA Updates | Wireless firmware update via Arduino IDE |
| 🔒 WPA2 AP | Secured hotspot with configurable password |
| 🏭 Factory Reset | One-tap reset from the System tab |
| 🌑 Captive Portal | Auto-redirects on first boot for easy setup |

---

## 🖥️ Butler GUI — Live Demo

**Try the web interface right in your browser:**

### 👉 [https://muhammednihalmp.github.io/WIFI-Extender/](https://muhammednihalmp.github.io/WIFI-Extender/)

The demo simulates the full GUI running on the ESP8266 including:
- **Status tab** — live stats (clients, RSSI, uptime, heap)
- **Network tab** — configure upstream STA and AP hotspot
- **Clients tab** — connected device list
- **Scan tab** — nearby WiFi networks
- **Advanced tab** — TX power slider, watchdog settings
- **System tab** — chip info, OTA, factory reset

---

## 🔌 Hardware

| Board | Notes |
|---|---|
| NodeMCU v1/v2/v3 | Recommended — has USB-serial built in |
| Wemos D1 Mini | Compact option |
| Generic ESP8266 | Any module with ≥ 4 MB flash |

---

## 📦 Requirements

### Arduino IDE settings

| Setting | Value |
|---|---|
| Board | NodeMCU 1.0 (ESP-12E) or Generic ESP8266 |
| Flash Size | **4MB (FS:2MB OTA:~1019 KB)** |
| CPU Frequency | **160 MHz** |
| lwIP Variant | **v2 Higher Bandwidth (no features)** ← required for NAT |
| Upload Speed | 921600 |

### Libraries (install via Library Manager)

- `ArduinoJson` by Benoit Blanchon — **v6.x**
- `ESP8266WiFi` — bundled with esp8266 core
- `ESP8266WebServer` — bundled
- `ESP8266mDNS` — bundled
- `ArduinoOTA` — bundled
- `LittleFS` — bundled
- `DNSServer` — bundled

### esp8266 Arduino core

```
https://arduino.esp8266.com/stable/package_esp8266com_index.json
```

Install via **File → Preferences → Additional Boards Manager URLs**, then install `esp8266` ≥ 3.1.0 from Boards Manager.

---

## 🚀 Quick Start

### 1. Flash the firmware

1. Clone or download this repo
2. Open `ESP8266_WiFi_Repeater_Butler.ino` in Arduino IDE
3. Select your board and set the options above
4. Click **Upload**

### 2. First-boot setup

1. On your phone or laptop, connect to the WiFi network:
   - **SSID:** `ESP8266_EXT`
   - **Password:** `12345678`
2. Open a browser and go to **[http://192.168.4.1](http://192.168.4.1)**
3. Go to the **Network** tab
4. Enter your home router SSID and password
5. Click **Save & Reconnect**

The ESP8266 will connect to your router and start forwarding traffic. All devices connected to `ESP8266_EXT` now have full internet access.

---

## 🌐 REST API

The Butler GUI communicates with the ESP8266 over a simple JSON REST API.

| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/status` | Full status JSON |
| GET | `/api/config` | Current config (no passwords) |
| GET | `/api/clients` | Connected AP stations |
| GET | `/api/scan` | Scan upstream networks |
| GET | `/api/heap` | Free heap bytes |
| POST | `/api/config/sta` | Set STA SSID + password |
| POST | `/api/config/ap` | Set AP SSID, password, channel |
| POST | `/api/power` | Set TX power (0–82 units) |
| POST | `/api/reboot` | Reboot ESP8266 |
| POST | `/api/factory` | Factory reset + reboot |

### Example — change upstream WiFi

```bash
curl -X POST http://192.168.4.1/api/config/sta \
  -H "Content-Type: application/json" \
  -d '{"staSsid":"MyRouter","staPass":"mypassword"}'
```

---

## ⚙️ Configuration (compile-time defaults)

Edit these at the top of the `.ino` file before flashing:

```cpp
#define DEFAULT_STA_SSID    ""               // upstream SSID (blank = captive portal)
#define DEFAULT_STA_PASS    ""               // upstream password
#define DEFAULT_AP_SSID     "ESP8266_EXT"    // your extender's SSID
#define DEFAULT_AP_PASS     "12345678"       // your extender's password
#define DEFAULT_AP_CHANNEL  6
#define DEFAULT_AP_MAXCONN  4
#define AP_IP               "192.168.4.1"

#define WATCHDOG_PING_HOST  "8.8.8.8"
#define WATCHDOG_INTERVAL_S 30
#define RECONNECT_RETRIES   10
#define HARD_REBOOT_MIN     5

#define OTA_HOSTNAME        "esp8266-repeater"
#define OTA_PASSWORD        "butler2024"
```

---

## 🔄 OTA (Wireless Update)

1. In Arduino IDE, go to **Tools → Port** — select the network port `esp8266-repeater`
2. Enter the OTA password: `butler2024`
3. Click **Upload** — no USB cable needed

---

## 🗂️ Project Structure

```
ESP8266_WiFi_Repeater_Butler/
├── ESP8266_WiFi_Repeater_Butler.ino   ← main firmware + Butler GUI
└── README.md                          ← this file
```

Config is stored at `/config.json` in LittleFS (not in the sketch directory).

---

## 🔧 Troubleshooting

| Problem | Fix |
|---|---|
| NAT not working | Set lwIP to **v2 Higher Bandwidth** in Tools |
| GUI not loading | Make sure Flash is set to 4MB with FS:2MB |
| Can't connect to upstream | Double-check SSID/password in Network tab |
| OTA not visible | Ensure device is on same network, mDNS working |
| Heap low / crashes | Reduce `apMaxConn`, avoid large scan results |

---

## 📄 License

MIT License — free to use, modify, and distribute.

---

## 👤 Author

**Muhammed Nihal MP**
GitHub: [@muhammednihalmp](https://github.com/muhammednihalmp)
Demo: [https://muhammednihalmp.github.io/WIFI-Extender/](https://muhammednihalmp.github.io/WIFI-Extender/)
