# 💧 IoT Pipeline Leak Detection System

An ESP32-based IoT system that detects water pipeline leakage by continuously monitoring inlet and outlet water flow rates. The system automatically switches OFF the water pump when a leak is detected and sends real-time alerts through Telegram.

## 🚀 Features

- Real-time water flow monitoring
- Dual flow sensor comparison (IN and OUT)
- Automatic pipeline leak detection
- Automatic pump shutdown during leakage
- Telegram notifications
- Live web dashboard
- OLED display for local monitoring
- HC-SR04 ultrasonic distance measurement
- Buzzer and LED leak alerts
- Wi-Fi connectivity and ESP32 access point
- Five-reading moving average for flow smoothing

## 🛠️ Hardware Components

- ESP32 Development Board
- 2 × Water Flow Sensors
- Relay Module
- DC Water Pump
- HC-SR04 Ultrasonic Sensor
- SSD1306 OLED Display (128 × 64)
- Buzzer
- LED
- Connecting wires and power supply

## 🔌 Pin Configuration

| Component | ESP32 GPIO |
|---|---|
| Flow Sensor IN | GPIO 25 |
| Flow Sensor OUT | GPIO 26 |
| Relay | GPIO 27 |
| Buzzer | GPIO 14 |
| LED | GPIO 2 |
| Ultrasonic TRIG | GPIO 32 |
| Ultrasonic ECHO | GPIO 33 |
| OLED SDA | GPIO 21 |
| OLED SCL | GPIO 22 |

**Important:** HC-SR04 ECHO must be connected through a voltage divider before connecting to ESP32 GPIO 33.

## ⚙️ Leak Detection Logic

- Flow readings are calculated every second.
- A five-reading moving average reduces measurement fluctuations.
- The system calculates the difference between inlet and outlet flow.
- Minimum inlet flow required: 1.00 L/min.
- Leak threshold: 0.70 L/min.
- Leak confirmation: 2 consecutive readings.
- Startup grace period: 10 seconds.

When a leak is confirmed:
1. The pump automatically switches OFF.
2. The buzzer and LED turn ON.
3. The OLED displays leak status.
4. Telegram sends a leak alert.
5. The web dashboard displays the leak condition.

## 📱 Monitoring

The system provides two monitoring methods:

**OLED Display:** Displays inlet flow, outlet flow, difference, ultrasonic distance and system status.

**Web Dashboard:** Displays live flow readings, leak status, pump status, Wi-Fi status and Telegram connection status.

ESP32 Access Point:
- SSID: `Pipeline-ESP32`
- Password: `12345678`
- Dashboard: `http://192.168.4.1`

## 📚 Software Requirements

- Arduino IDE
- ESP32 Board Package
- Adafruit GFX Library
- Adafruit SSD1306 Library
- Wi-Fi and HTTPClient libraries included with ESP32 Arduino Core

## 🔧 Installation

1. Install Arduino IDE.
2. Install the ESP32 board package.
3. Install the required Adafruit libraries.
4. Open `IoT-Pipeline-Leak-Detection.ino`.
5. Configure Wi-Fi credentials and Telegram bot settings.
6. Select your ESP32 board and COM port.
7. Upload the program.
8. Connect to the ESP32 access point or configured Wi-Fi network.
9. Open the dashboard in your browser.

## 🔐 Security Note

Do not publish real Wi-Fi passwords, Telegram bot tokens or private credentials in public repositories.

## 🔮 Future Improvements

- Machine learning-based leak prediction
- Cloud-based data storage
- Historical flow analysis
- Advanced analytics dashboard
- Remote pump control

## 👨‍💻 Developer

**Nithyapriyan R.**

GitHub: [@Nithyapriyan2007](https://github.com/Nithyapriyan2007)

---

⭐ If you find this project useful, consider giving the repository a star!
