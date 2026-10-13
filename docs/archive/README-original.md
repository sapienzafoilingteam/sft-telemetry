> Appunti storici: percorsi e parametri possono essere superati. Vedi il README della repository.

# ESP32-S3 IMU & GPS Diagnostic System

Questo progetto è un sistema di diagnostica e acquisizione dati basato su ESP32-S3, progettato per interfacciare una IMU (Inertial Measurement Unit) e un modulo GPS. Il sistema calcola l'orientamento (Roll, Pitch, Yaw) in tempo reale e monitora il segnale satellitare.

## 🚀 Caratteristiche principali
- **Scansione I2C automatica:** Verifica la presenza dei sensori all'avvio.
- **Auto-Calibrazione Gyro:** Routine di 2 secondi all'accensione per azzerare il bias del giroscopio.
- **Fusione Sensori:** Algoritmo di Madgwick per il calcolo dell'assetto a 50Hz.
- **Monitoraggio GPS:** Parsing dei dati NMEA tramite TinyGPS++.
- **Feedback LED:** Segnalazione visiva dello stato dei sensori tramite LED di diagnostica.
- **Visualizzazione Web:** Bridge WebSocket incluso per visualizzare i dati in 3D e su mappa.

## 🛠 Componenti Hardware
| Componente | Modello Specifico | Protocollo |
| :--- | :--- | :--- |
| **Microcontrollore** | ESP32-S3 (DevKitC-1) | - |
| **IMU (Accel/Giro)** | Pololu MinIMU-9 v5 (LSM6DS33) | I2C |
| **Magnetometro** | Pololu MinIMU-9 v5 (LIS3MDL) | I2C |
| **GPS** | NEO-6M (o compatibile) | UART |

## 🔌 Collegamenti (Pinout)
### Sensori & LED
- **I2C (IMU):**
    - **SDA:** GPIO 8
    - **SCL:** GPIO 9
- **UART (GPS):**
    - **GPS TX → ESP RX:** GPIO 18
    - **GPS RX → ESP TX:** GPIO 17
- **LED di Stato:**
    - **LED 1 (IMU Status):** GPIO 21
    - **LED 2 (Mag Status):** GPIO 45

## 💻 Architettura del Codice
Il firmware è sviluppato in C++ (Arduino framework) su **PlatformIO**.

### Moduli Principali:
1.  **Inizializzazione:**
    - Scansione del bus I2C per gli indirizzi `0x6B` (LSM6) e `0x1E` (LIS3MDL).
    - Se l'IMU non viene trovata, il sistema entra in modalità errore (LED 1 lampeggiante).
2.  **Calibrazione:**
    - All'avvio, il sistema campiona il giroscopio per 2000ms per calcolare l'offset statico. Durante questa fase è necessario tenere la scheda ferma.
3.  **Loop di Esecuzione:**
    - **Task 50Hz (20ms):** Lettura IMU + Filtro di Madgwick. Output seriale formato `DATA:Roll,Pitch,Yaw`.
    - **Task 1Hz (1s):** Statistiche GPS. Output seriale `GPS_STATS`.
    - **GPS Background:** Parsing continuo dei caratteri ricevuti sulla Serial1.

## 📦 Librerie Utilizzate
- `LSM6` (Pololu)
- `LIS3MDL` (Pololu)
- `Madgwick` (Arduino)
- `TinyGPSPlus` (Mikal Hart)
- `Wire` (Built-in I2C)

## 🖥 Visualizzatori Web
Il progetto include due strumenti di visualizzazione che leggono i dati tramite `server.py`:
- `visualizer.html`: Orientamento 3D della scheda (Three.js).
- `visualizer_gps.html`: Coordinate, altitudine e stato satelliti.

---
*Progetto sviluppato per il Sapienza Foiling Team.*
