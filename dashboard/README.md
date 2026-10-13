# Dashboard e bridge

Avvia `python dashboard/serverw.py` dalla radice: il server imposta la directory di lavoro su `dashboard/`, quindi i collegamenti relativi a librerie, mappe e sessioni rimangono validi.

| Pagina | Uso |
| --- | --- |
| `visualizerw.html` | Telemetria UDP del firmware SD |
| `visualizer_nosd.html` | Variante con flash interna |
| `visualizer_sd.html` | Comandi e stato dello storage |
| `visualizer_gps.html` | Pannello GPS storico; compatibilità da verificare con la variante scelta |
| `playback.html` | Riproduzione di CSV locali |
| `web_logger/index.html` | Logger browser sperimentale |

Il computer deve essere connesso all'access point dell'ESP32. Premi il pulsante di connessione al bridge nella pagina. Porte: HTTP 8000, WebSocket 8001, UDP 4210. Il bridge ascolta sulla rete locale e non richiede autenticazione: usalo sulla rete di prova del team.

Le librerie JS/CSS sono incluse in `libs/`. Font esterni e mappe non presenti in cache possono richiedere Internet. `map_tiles/` è una cache locale ignorata; `sessions/` contiene i file acquisiti o scaricati dalla scheda ed è anch'essa ignorata.

Il pre-scaricamento massivo delle mappe è disattivato all'avvio. L'opzione storica `SFT_PREFETCH_MAPS=1 python dashboard/serverw.py` lo riattiva; verifica il provider e la sua politica prima di usarla. Il server può ancora richiedere le singole tile visualizzate quando non sono già in cache.

Per il playback su un clone nuovo copia i tuoi CSV in `dashboard/sessions/` e riavvia/riconnetti il bridge. Nessuna sessione GPS reale è distribuita nella repository pubblica.
