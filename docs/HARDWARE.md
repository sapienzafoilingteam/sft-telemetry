# Hardware e protocolli

Questa tabella è ricavata dal sorgente attuale di `firmware/telemetry-sd/src/main.cpp`, non dai vecchi appunti.

| Collegamento | GPIO ESP32-S3 |
| --- | --- |
| I2C SDA / SCL | 8 / 9 |
| GPS TX → ESP RX / GPS RX ← ESP TX | 1 / 2 |
| SD CS / MOSI / SCK / MISO | 10 / 11 / 12 / 13 |
| LED IMU / magnetometro | 21 / 45 |
| Pulsante Wi-Fi / LED Wi-Fi | 5 / 4 |
| Pulsante registrazione / LED registrazione | 7 / 6 |
| Pulsante reset / LED reset | 18 / 17 |

I2C: LSM6 e LIS3MDL, con indirizzi rilevati nelle prove `0x6B` e `0x1E`. I vecchi appunti che assegnano 18/17 al GPS non descrivono questo firmware. Controlla i `#define` di ogni variante prima di cablarla.

Nel firmware SD la fusione IMU è configurata a 50 Hz; la calibrazione iniziale del giroscopio dura 3 secondi e richiede scheda ferma. Il GPS viene letto in continuo e i dati sono aggiornati ogni secondo. Il bridge UDP usa 4210 e l'ESP32 espone anche HTTP su 80 per i file. Il browser-logger usa invece HTTP 80 e WebSocket 81 e non richiede il bridge Python.

Le partizioni delle varianti LittleFS occupano fino a 8 MB: controlla la capacità effettiva della scheda e la configurazione prima del caricamento. Un `uploadfs` sostituisce il contenuto del filesystem e può cancellare registrazioni locali della variante con flash.

Per la microSD, l'assenza di risposta CMD0 precede l'accesso al filesystem: non prova un problema di formattazione. Segui [la diagnosi conservata](../diagnostics/sd_usb_probe/README.md) per continuità, alimentazione, modulo e scheda nota funzionante.
