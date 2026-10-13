# Diagnostica hardware

Ogni cartella contiene un progetto PlatformIO indipendente. Compila dalla radice con `pio run -d diagnostics/<nome>`.

| Progetto | Scopo |
| --- | --- |
| [gps_debug](gps_debug) | UART GPS: recherche del baud rate e dati grezzi. |
| [gps_only_test](gps_only_test) | Ricezione GPS isolata e parsing TinyGPS++. |
| [hw_test](hw_test) | Pannello completo per LED, I2C, IMU, GPS e SD. |
| [simplesd](simplesd) | Probe SPI minimo. |
| [sd-test](sd-test) | Ricerca delle permutazioni dei pin SPI (SdFat). |
| [sd_debug](sd_debug) | Diagnostica SD con prove di lettura/scrittura. |
| [sd_only_test](sd_only_test) | Diagnostica SD isolata, GPIO e SdFat. |
| [sd_tutorial_test](sd_tutorial_test) | Esempio SD con gestione di file e directory. |
| [sd_usb_probe](sd_usb_probe) | Sonda CMD0 e mount senza scrittura; evidenze USB conservate. |
| [sdtest](sdtest) | Variante di diagnostica SD con Wi-Fi; test_sdfat.cpp è uno sketch ausiliario fuori da src/. |

Prima di caricare, controlla i pin nel sorgente e le operazioni sulla SD: diversi test scrivono file o includono formattazione. Il caricamento sostituisce il firmware in esecuzione. I risultati conservati per sd_usb_probe descrivono la prova del 1 ottobre 2026, non una verifica attuale di ogni modulo.
