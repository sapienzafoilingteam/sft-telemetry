# Logger nel browser, senza microSD

L'ESP32 espone l'app da LittleFS su HTTP 80 e invia IMU/GPS via WebSocket 81. Il browser accumula i campioni in RAM e permette di scaricare un CSV. Questa variante non usa il bridge Python.

Dalla radice della repository:

```bash
cp firmware/browser-logger/src/secrets.h.example firmware/browser-logger/src/secrets.h
# Imposta la password Wi-Fi nel file appena copiato.
pio run -d firmware/browser-logger
pio run -d firmware/browser-logger --target buildfs
# Dopo aver controllato la scheda e salvato eventuali dati locali:
pio run -d firmware/browser-logger --target uploadfs
pio run -d firmware/browser-logger --target upload
```

La tabella partizioni richiede 8 MB di flash. L'upload del filesystem sostituisce il contenuto precedente. Collega telefono o computer a `SFT_telemetry`, poi apri `http://192.168.4.1`. Usa i pulsanti di registrazione e scarica il CSV prima di chiudere o ricaricare la pagina.

L'app, Leaflet e le immagini dei marker sono inclusi in `data/`. Le tile delle mappe sono escluse da Git: un clone nuovo non include una mappa offline. Il vecchio `download_tiles.py` conserva la procedura sperimentale di generazione della cache per il Lago di Garda; controlla dimensioni della partizione e politica del provider prima di eseguirlo dalla cartella di questa variante. L'HTML può essere caricato anche senza tile, ma la mappa locale risulterà incompleta.

La compilazione e la generazione del filesystem sono state verificate nella riorganizzazione. Funzionamento su hardware, affidabilità delle registrazioni e compatibilità dei singoli browser richiedono prove dedicate.
