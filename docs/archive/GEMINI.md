> Appunti storici: percorsi e parametri possono essere superati. Vedi il README della repository.

# Project Overview: ESP32-S3 IMU & GPS Test Panel

This project is a diagnostic and verification tool for an ESP32-S3 based system. It is designed to verify hardware connections (solder joints and breadboard wiring) for an Inertial Measurement Unit (IMU) and a GPS module.

## Technologies and Frameworks
- **Hardware:** ESP32-S3 (DevKitC-1)
- **Framework:** Arduino
- **Build System:** PlatformIO
- **Peripherals:** 
    - **IMU:** Pololu MinIMU-9 v5 (LSM6DS33 Accel/Gyro + LIS3MDL Magnetometer) via I2C.
    - **GPS:** NEO-6M (or compatible) via UART.

## Hardware Configuration (Pinout)
- **I2C (IMU):**
    - SDA: GPIO 8
    - SCL: GPIO 9
- **LEDs:**
    - LED 1: GPIO 21 (Indicates IMU status)
    - LED 2: GPIO 45 (Indicates Magnetometer status)
- **UART (GPS):**
    - GPS TX -> ESP RX: GPIO 1
    - GPS RX -> ESP TX: GPIO 2
    - Baud Rate: 9600

## Architecture
- **I2C Scanner:** On startup, the firmware scans the I2C bus to detect the LSM6 (0x6B) and LIS3MDL (0x1E).
- **Initialization:** Sensors are initialized if found. Failure to find a sensor is reported via Serial.
- **Real-time Monitoring:** The `loop()` function continuously reads data from the IMU, Magnetometer, and GPS, outputting it to the Serial Monitor at 5Hz.

## Visualizers
The project includes two web-based visualizers:
1. **3D IMU Visualizer (`visualizer.html`):** Displays orientation using Three.js.
2. **GPS Test Panel (`visualizer_gps.html`):** Displays real-time GPS coordinates, altitude, speed, and satellite count.

### **Usage:**
1. **Run the Server & Bridge:** Both visualizers require the serial-to-websocket bridge. You can run the unified server:
   ```bash
   python3 server.py
   ```
   This starts an HTTP server on port 8000 and a WebSocket bridge on port 8001.
2. **Access:**
   - For IMU: `http://localhost:8000/visualizer.html`
   - For GPS: `http://localhost:8000/visualizer_gps.html`
3. **Connect:** Click the "Connect" button in the browser to start receiving data.

## Building and Running
The project uses PlatformIO. Use the following commands:

- **Build Project:** `pio run`
- **Upload Firmware:** `pio run --target upload`
- **Monitor Serial Output:** `pio device monitor` (Baud rate: 115200)
- **Clean Build:** `pio run --target clean`

## Testing Guidelines
Refer to `INSTRUCTIONS_TEST.md` for a detailed post-soldering verification guide. Key checks include:
1. **I2C Detection:** Confirm addresses `0x6B` and `0x1E` are found.
2. **GPS Fix:** Verify serial data is received (`gps.charsProcessed() > 0`). Satellites and location fix may require outdoor exposure.
3. **Sensor Response:** Ensure `A(x,y,z)`, `G(x,y,z)`, and `M(x,y,z)` values react to board movement.

## Development Conventions
- **Serial Debugging:** Uses the native USB CDC port (`ARDUINO_USB_CDC_ON_BOOT=1`).
- **Update Rate:** Sensor data is printed at 50Hz (every 20ms) for IMU and 1Hz for GPS.
- **Libraries:** Dependencies are managed in `platformio.ini` (LSM6, LIS3MDL, TinyGPSPlus).
