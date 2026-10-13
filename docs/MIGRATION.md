# Riorganizzazione del 13 ottobre 2026

Il lavoro precedente è stato raccolto in questa repository senza scegliere arbitrariamente una variante unica. [migration-map.json](migration-map.json) riporta la corrispondenza tra percorsi originali e attuali.

La cartella originale completa, inclusa la vecchia `.git`, il suo indice con modifiche già preparate, tutti i sorgenti, le registrazioni e gli artefatti, è conservata localmente in una cartella sorella `esp32-imu-backup-20261013-120059`. Non è stata riscritta né pubblicata la vecchia cronologia: conteneva artefatti di compilazione e registrazioni.

La repository pubblica parte da una nuova cronologia pulita. I CSV/JSONL sono rimasti in `dashboard/sessions/` sul computer originale; mappe e copie della flash sono anch'esse locali e ignorate. `migration-local.json` è un inventario locale con percorso del backup e hash dei file originali, utile per confrontare o ripristinare il lavoro.

Modifiche oltre ai percorsi: password Wi-Fi estratta in `secrets.h` locali; password dimostrativa per i clone; versione della piattaforma PlatformIO fissata a quella installata durante la verifica; dipendenze Python dichiarate; asset Leaflet aggiunti al filesystem del browser-logger; download anticipato delle mappe reso facoltativo. Il test storico sdtest usava la macro non disponibile SD_SCK_KHZ: è stata sostituita con frequenze equivalenti espresse in Hz (400000 e 100000). Gli algoritmi di acquisizione e i protocolli delle varianti sono stati conservati.

I vecchi README e appunti sono in `docs/archive/` e descrivono uno stato storico. Per le istruzioni attuali usa il README principale e i documenti delle singole aree.
