# Sketch Arduino intermedi

Gli sketch `sd_init_fixed` e `sft_wifi_debug` conservano versioni intermedie del firmware. Ogni `.ino` è dentro una cartella omonima per essere aperto nell'IDE Arduino. Non sono il percorso principale di compilazione PlatformIO e non sono coperti dalla CI.

Le password sono configurabili copiando il `secrets.h.example` della stessa cartella in `secrets.h`. Le dipendenze vanno installate nell'IDE Arduino; per il lavoro corrente parti dai progetti in `firmware/`.
