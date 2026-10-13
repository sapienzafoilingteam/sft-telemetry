# Verifiche della riorganizzazione

Verifiche locali del 13 ottobre 2026:

- Compilazione dei 13 progetti PlatformIO con ESP32 platform 6.13.0.
- Generazione LittleFS del browser-logger.
- Parsing e compilazione sintattica dei file Python.
- Avvio del bridge senza pre-scaricamento delle mappe; risposta HTTP delle dashboard e librerie; handshake e ping WebSocket.
- Controllo dei collegamenti nella documentazione principale.
- Confronto dell'inventario locale: tutti i 56829 file originali copiati presenti nei percorsi di destinazione.
- Controllo dei file pubblicati: nessuna vecchia password, sessione GPS, cache, compilazione o copia completa della flash.

Non sono stati caricati firmware sulla scheda durante questa riorganizzazione. Nessuna nuova prova fisica di IMU, GPS, SD o registrazione end-to-end è implicata da queste verifiche. Le evidenze USB della sonda sono quelle storiche del 1 ottobre.

Gli sketch Arduino intermedi non sono stati compilati nell'IDE Arduino. La CI controlla i tre firmware principali, sd_usb_probe e la sintassi Python a ogni push e pull request.
