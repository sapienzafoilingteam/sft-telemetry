# Contribuire a SFT Telemetry

## Scegli la variante

Parti dalla tabella nel README. `telemetry-sd`, `telemetry-nosd` e `browser-logger` sono applicazioni distinte: una modifica in una variante non viene applicata automaticamente alle altre. Mantieni gli esperimenti separati dal firmware principale.

## Ambiente e workflow

1. Clona la repository e installa le dipendenze indicate nel README.
2. Crea un branch descrittivo, per esempio `git switch -c fix/gps-parser`.
3. Configura la password nel `secrets.h` locale del progetto interessato.
4. Compila con `pio run -d <cartella-del-progetto>`.
5. Per modifiche al browser-logger compila anche il filesystem con `pio run -d firmware/browser-logger --target buildfs`.
6. Per modifiche Python verifica con `python -m compileall -q dashboard tools firmware/browser-logger/download_tiles.py`.
7. Apri una pull request su `main`, descrivendo problema, variante, verifica e limiti.

I membri dell'organizzazione con permesso di scrittura possono usare branch nella repository; gli altri possono proporre una pull request da un fork. I permessi sono gestiti dagli amministratori dell'organizzazione.

## Come documentare una prova

Indica scheda e revisione PCB, variante, collegamenti effettivi, versione delle dipendenze, alimentazione e risultato osservato. Distingui sempre compilazione, caricamento, collegamento dei sensori e prova reale. Se cambi pin, porte, formato CSV o comandi UDP/WebSocket, aggiorna la documentazione collegata.

Per i test SD leggi prima il sorgente: alcuni scrivono file o espongono comandi di formattazione. La sonda `sd_usb_probe` è pensata per controlli senza scrittura sulla scheda.

## File da condividere

Condividi sorgenti, configurazioni, documentazione e log diagnostici senza dati personali. Non aggiungere registrazioni GPS reali, `secrets.h`, backup della flash o cache. Per riprodurre un problema con dati di esempio crea un campione sintetico dichiarato come tale.

Le librerie web in `dashboard/libs` mantengono i propri avvisi di licenza. Non rimuoverli. Una licenza generale per il codice del team non è stata aggiunta durante la riorganizzazione: la scelta spetta ai responsabili del progetto.
