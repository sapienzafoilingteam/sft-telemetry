# SFT Telemetry

Firmware ESP32-S3, dashboard web e strumenti di diagnostica per la telemetria del Sapienza Foiling Team. Questa repository raccoglie il lavoro su IMU, GPS, registrazione e microSD; le varianti rimangono separate perché usano protocolli e modalità di salvataggio differenti.

## Da dove iniziare

| Obiettivo | Progetto | Funzionamento |
| --- | --- | --- |
| Telemetria e registrazione su microSD | [firmware/telemetry-sd](firmware/telemetry-sd) | IMU/GPS → UDP → dashboard; CSV su SD |
| Registrare nella flash interna senza SD | [firmware/telemetry-nosd](firmware/telemetry-nosd) | IMU/GPS → UDP → dashboard; CSV in LittleFS |
| Registrare dal browser del telefono | [firmware/browser-logger](firmware/browser-logger) | HTTP/WebSocket dall'ESP32; CSV nel browser |
| Vedere i dati e riprodurre sessioni | [dashboard](dashboard/README.md) | Bridge Python, visualizzatori, playback |
| Verificare cablaggio, GPS e SD | [diagnostics](diagnostics/README.md) | Test PlatformIO indipendenti |
| Consultare gli sketch intermedi | [experiments/arduino](experiments/README.md) | Versioni Arduino conservate come riferimento |

## Prima compilazione

Installa Python 3.10+ e PlatformIO Core (oppure l'estensione PlatformIO per VS Code). Dalla radice della repository:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements-dev.txt
cp firmware/telemetry-sd/src/secrets.h.example firmware/telemetry-sd/src/secrets.h
# Modifica secrets.h e scegli la password del tuo access point (almeno 8 caratteri).
pio run -d firmware/telemetry-sd
```

Gli altri progetti hanno un `platformio.ini` autonomo: usa `pio run -d <cartella>`. Nei firmware Wi-Fi copia il rispettivo `src/secrets.h.example` prima del primo caricamento. Senza file locale il firmware usa la password dimostrativa `change-me-now`.

Per caricare il firmware e leggere il monitor seriale:

```bash
pio run -d firmware/telemetry-sd --target upload
pio device monitor --baud 115200
```

Il caricamento sostituisce l'applicazione presente sulla scheda. Verifica la variante e il cablaggio prima di eseguirlo; la sola compilazione non dimostra che i sensori funzionino.

## Dashboard sul computer

```bash
python -m pip install -r dashboard/requirements.txt
python dashboard/serverw.py
```

Collega il computer al Wi-Fi `SFT_telemetry` con la password configurata. Apri [visualizerw](http://localhost:8000/visualizerw.html), oppure [playback](http://localhost:8000/playback.html). Il bridge ascolta UDP su 4210, HTTP su 8000 e WebSocket su 8001. [Istruzioni e varianti](dashboard/README.md).

## Hardware e stato del lavoro

Il firmware principale usa ESP32-S3 DevKitC-1, LSM6, LIS3MDL e GPS UART. Il suo cablaggio attuale usa I2C 8/9, GPS 1/2 e SD 10/11/12/13; le varianti diagnostiche vanno controllate singolarmente. [Pin e protocolli](docs/HARDWARE.md).

La microSD resta un problema hardware da verificare: le prove USB conservate del 1 ottobre 2026 mostrano assenza di risposta a CMD0 (`R1=0xFF`). Non costituiscono una conferma del funzionamento della registrazione SD. [Evidenze della diagnosi](diagnostics/sd_usb_probe/README.md).

## Contribuire

Leggi [CONTRIBUTING.md](CONTRIBUTING.md): indica la variante su cui lavori, modifica un branch e apri una pull request con compilazione e prove effettuate. I controlli automatici compilano i tre firmware principali e la sonda SD e verificano i file Python.

Registrazioni GPS, credenziali, backup della flash, cache delle mappe e compilazioni sono esclusi dalla repository pubblica. I file locali restano disponibili nelle cartelle ignorate. [Riorganizzazione e conservazione del lavoro originale](docs/MIGRATION.md).
