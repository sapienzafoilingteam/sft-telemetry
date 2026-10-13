1. Spiegazione del problema dell'alimentazione: Quando la USB è staccata, il pin MISO va a HIGH semplicemente perché la scheda SD è spenta (manca il 5V). Il "conflitto" era in realtà il level shifter della scheda SD che forzava lo stato LOW, il che è normale.
2. Fix SPI: `sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);` invece di passare `SD_CS`. Passare `SD_CS` assegna il pin all'hardware SPI, impedendo alla libreria `SD` di gestirlo come GPIO per il Chip Select.
3. Fix bitbang CMD0: Aggiungiamo CMD8 e leggiamo le risposte in modo più robusto.
