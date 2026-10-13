import asyncio
import websockets
import socket
import os
import sys
import threading
import json
import time
import math
import urllib.request
from datetime import datetime
from http.server import SimpleHTTPRequestHandler
from socketserver import TCPServer

# ==========================================
# CONFIGURAZIONE DI RETE
# ==========================================
HTTP_PORT = 8000
WS_PORT = 8001
UDP_IP = "0.0.0.0" # 0.0.0.0 significa "Ascolta su tutte le interfacce Wi-Fi del PC"
UDP_PORT = 4210    # Deve coincidere con il targetPort nel codice dell'ESP32

# ==========================================
# CONFIGURAZIONE CACHE MAPPA OFFLINE
# ==========================================
# Area da pre-scaricare: lat/lon min e max + livelli di zoom
# Area: Lago di Garda e dintorni (zona rilevata dal GPS: 45.7987, 10.8382)
MAP_CACHE_DIR = "map_tiles"          # cartella locale dove salvare le tile
MAP_LAT_MIN  = 45.40                 # latitudine sud dell'area (sotto Desenzano)
MAP_LAT_MAX  = 46.10                 # latitudine nord dell'area (Riva del Garda)
MAP_LON_MIN  = 10.45                 # longitudine ovest (Salò / sponda ovest)
MAP_LON_MAX  = 11.00                 # longitudine est (Peschiera / sponda est)
MAP_ZOOM_MIN = 10                    # zoom minimo (visione larga del lago)
MAP_ZOOM_MAX = 18                    # zoom massimo (16 = ottimo per navigazione)
OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
MAP_USER_AGENT = "SFT-Telemetry/1.0 (Sapienza Foiling Team; telemetry@sapienzafoilingteam.it)"

# Coda thread-safe per scambiare dati tra il thread UDP e il thread WebSocket
message_queue = None
asyncio_loop = None

# Indirizzo dell'ESP32 (rilevato automaticamente dall'ultimo pacchetto UDP)
esp32_addr = None
# Socket globale per inviare comandi UDP all'ESP32
udp_send_sock = None

# Stato IMU (aggiornato dal thread WebSocket)
last_imu_roll = 0.0
last_imu_pitch = 0.0
last_imu_yaw = 0.0

# Zero di riferimento IMU (impostato dall'utente prima della regata)
imu_zero_roll = 0.0
imu_zero_pitch = 0.0
imu_zero_yaw = 0.0

# Stato GPS (aggiornato in tempo reale, 1Hz)
last_gps_lat = 0.0
last_gps_lng = 0.0
last_gps_sats = 0

# Registrazione CSV su PC
pc_csv_recording = False
pc_csv_file = None
pc_csv_filename = ""
pc_csv_start_time = None
pc_csv_session_num = 0
pc_csv_row_count = 0

# ==========================================
# CACHE MAPPA TILE
# ==========================================

def _deg2tile(lat_deg, lon_deg, zoom):
    """Converte coordinate geografiche in indici tile OSM (x, y)."""
    lat_r = math.radians(lat_deg)
    n = 2 ** zoom
    x = int((lon_deg + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return x, y

def _tile_path(z, x, y):
    """Percorso locale della tile."""
    return os.path.join(MAP_CACHE_DIR, str(z), str(x), f"{y}.png")

def _download_tile(z, x, y, subdomain_idx=0):
    """Scarica una singola tile da OpenStreetMap e la salva su disco. Ritorna True se ok."""
    path = _tile_path(z, x, y)
    if os.path.exists(path):
        return True  # già in cache
    os.makedirs(os.path.dirname(path), exist_ok=True)
    url = f"https://tile.openstreetmap.de/{z}/{x}/{y}.png"
    try:
        req = urllib.request.Request(url, headers={
            "User-Agent": MAP_USER_AGENT,
            "Referer": "https://sapienzafoilingteam.it/"
        })
        with urllib.request.urlopen(req, timeout=10) as resp:
            with open(path, "wb") as f:
                f.write(resp.read())
        return True
    except Exception:
        return False

def prefetch_map_tiles():
    """
    Pre-scarica le tile OSM per l'area configurata a tutti i livelli di zoom.
    Viene eseguita in un thread separato all'avvio.
    """
    total = 0
    downloaded = 0
    skipped = 0
    failed = 0

    # Calcola il numero totale di tile
    for z in range(MAP_ZOOM_MIN, MAP_ZOOM_MAX + 1):
        x0, y0 = _deg2tile(MAP_LAT_MAX, MAP_LON_MIN, z)  # angolo NW
        x1, y1 = _deg2tile(MAP_LAT_MIN, MAP_LON_MAX, z)  # angolo SE
        total += (x1 - x0 + 1) * (y1 - y0 + 1)

    print(f"[MAP] Pre-download mappa offline: {total} tile da scaricare (zoom {MAP_ZOOM_MIN}-{MAP_ZOOM_MAX})")
    idx = 0
    for z in range(MAP_ZOOM_MIN, MAP_ZOOM_MAX + 1):
        x0, y0 = _deg2tile(MAP_LAT_MAX, MAP_LON_MIN, z)
        x1, y1 = _deg2tile(MAP_LAT_MIN, MAP_LON_MAX, z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                path = _tile_path(z, x, y)
                if os.path.exists(path):
                    skipped += 1
                else:
                    ok = _download_tile(z, x, y, idx)
                    if ok:
                        downloaded += 1
                    else:
                        failed += 1
                    idx += 1
                    time.sleep(0.15)  # delay cortese verso il tile server (≥150ms)
        print(f"[MAP] Zoom {z} completato — scaricate: {downloaded}, in cache: {skipped}, errori: {failed}")

    print(f"[MAP] ✅ Cache mappa pronta: {downloaded} scaricate, {skipped} già in cache, {failed} errori.")


def run_http_server():
    """Server statico per ospitare il file HTML del Visualizer e le tile della mappa."""
    os.chdir(os.path.dirname(os.path.abspath(__file__)))

    class MyHandler(SimpleHTTPRequestHandler):
        def log_message(self, format, *args):
            pass  # Nascondiamo i log di sistema per tenere il terminale pulito

        def do_GET(self):
            # Intercettiamo le richieste di tile mappa: /tiles/{z}/{x}/{y}.png
            if self.path.startswith("/tiles/"):
                parts = self.path.strip("/").split("/")
                try:
                    z, x, y_png = int(parts[1]), int(parts[2]), parts[3]
                    y = int(y_png.replace(".png", ""))
                    local_path = _tile_path(z, x, y)

                    if os.path.exists(local_path):
                        # Serviamo dalla cache locale
                        with open(local_path, "rb") as f:
                            data = f.read()
                        self.send_response(200)
                        self.send_header("Content-Type", "image/png")
                        self.send_header("Content-Length", str(len(data)))
                        self.send_header("Cache-Control", "public, max-age=86400")
                        self.end_headers()
                        self.wfile.write(data)
                    else:
                        # Non in cache: proviamo a scaricarla al volo (se online)
                        ok = _download_tile(z, x, y)
                        if ok:
                            with open(local_path, "rb") as f:
                                data = f.read()
                            self.send_response(200)
                            self.send_header("Content-Type", "image/png")
                            self.send_header("Content-Length", str(len(data)))
                            self.end_headers()
                            self.wfile.write(data)
                        else:
                            self.send_response(404)
                            self.end_headers()
                except Exception:
                    self.send_response(500)
                    self.end_headers()
                return
            # Tutti gli altri file: comportamento standard
            super().do_GET()

    TCPServer.allow_reuse_address = True
    try:
        with TCPServer(("", HTTP_PORT), MyHandler) as httpd:
            print(f"[HTTP] Server attivo su porta {HTTP_PORT}")
            httpd.serve_forever()
    except Exception as e:
        print(f"[HTTP] Errore Server: {e}")

def send_command_to_esp32(command):
    """Invia un comando UDP all'ESP32 (se il suo indirizzo è noto)"""
    global esp32_addr, udp_send_sock
    if esp32_addr and udp_send_sock:
        try:
            # Inviamo alla porta dell'ESP32 (stessa porta UDP)
            udp_send_sock.sendto(command.encode('utf-8'), esp32_addr)
            print(f"[UDP->ESP32] Comando inviato: {command} -> {esp32_addr}")
        except Exception as e:
            print(f"[UDP->ESP32] Errore invio comando: {e}")
    else:
        print(f"[UDP->ESP32] WARN: Indirizzo ESP32 sconosciuto, impossibile inviare '{command}'")

def udp_listener():
    """
    Funzione in esecuzione su un Thread separato.
    Cattura i pacchetti UDP ad altissima frequenza e li spinge nella coda Asyncio.
    """
    global esp32_addr, udp_send_sock
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((UDP_IP, UDP_PORT))
    
    # Socket per inviare comandi all'ESP32
    udp_send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    print(f"[UDP] In ascolto dei pacchetti telemetrici dall'ESP32 sulla porta {UDP_PORT}...")

    while True:
        try:
            # Buffer di 1024 bytes (più che sufficiente per le nostre stringhe)
            data, addr = sock.recvfrom(1024)
            msg = data.decode('utf-8', errors='ignore').strip()
            
            # Traccia l'indirizzo dell'ESP32 (aggiorna ad ogni pacchetto)
            if addr and addr != esp32_addr:
                esp32_addr = addr
                print(f"[UDP] ESP32 rilevato: {esp32_addr}")

            # Invia il messaggio alla coda asyncio (il CSV viene scritto lì)
            if msg and asyncio_loop and message_queue:
                asyncio_loop.call_soon_threadsafe(message_queue.put_nowait, msg)
        except Exception as e:
            print(f"[UDP] Errore Ricezione: {e}")

async def download_file_from_esp32(esp_ip, filename):
    """Scarica il file via HTTP dall'ESP32 e lo salva in sessions/ in background"""
    url = f"http://{esp_ip}/download?file={filename}"
    try:
        await message_queue.put(f"STATUS:SD_DWN_START:{filename}")
        loop = asyncio.get_running_loop()
        def do_download():
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=60) as response:
                content = response.read()
                save_name = filename.strip("/")
                os.makedirs("sessions", exist_ok=True)
                with open(f"sessions/{save_name}", "wb") as f:
                    f.write(content)
        await loop.run_in_executor(None, do_download)
        await message_queue.put(f"STATUS:SD_DWN_OK:{filename}")
    except Exception as e:
        await message_queue.put(f"STATUS:SD_DWN_ERROR:{filename}|{str(e)}")

async def ws_handler(websocket):
    """
    Gestore asincrono del WebSocket con gestione robusta della connessione.
    """
    print(f"[WS] Visualizer connesso da {websocket.remote_address}")
    
    # Task per inviare dati dalla coda al WebSocket
    async def send_data_task():
        global last_imu_roll, last_imu_pitch, last_imu_yaw
        global last_gps_lat, last_gps_lng, last_gps_sats
        global pc_csv_row_count
        try:
            while True:
                msg = await message_queue.get()
                
                ws_msg = msg
                if msg.startswith("IMU,"):
                    parts = msg.split(",")
                    try:
                        last_imu_roll  = float(parts[1])
                        last_imu_pitch = float(parts[2])
                        last_imu_yaw   = float(parts[3])
                    except (ValueError, IndexError):
                        pass
                    ws_msg = msg.replace("IMU,", "DATA:", 1)

                    # Scrittura CSV su PC durante la registrazione
                    if pc_csv_recording and pc_csv_file:
                        elapsed = time.time() - pc_csv_start_time
                        r = last_imu_roll  - imu_zero_roll
                        p = last_imu_pitch - imu_zero_pitch
                        # Normalizza la differenza di yaw in [-180, 180]
                        y_raw = last_imu_yaw - imu_zero_yaw
                        y = ((y_raw + 180) % 360) - 180
                        ts = datetime.now().strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]
                        pc_csv_file.write(
                            f"{ts},{elapsed:.3f},{r:.2f},{p:.2f},{y:.2f},"
                            f"{last_gps_lat:.6f},{last_gps_lng:.6f},{last_gps_sats}\n"
                        )
                        pc_csv_row_count += 1
                        if pc_csv_row_count % 50 == 0:  # flush ogni ~1 secondo
                            pc_csv_file.flush()

                elif msg.startswith("GPS,"):
                    parts = msg.split(",")
                    sats_str = parts[1] if len(parts) > 1 else "0"
                    lat_str  = parts[2] if len(parts) > 2 else "0"
                    lng_str  = parts[3] if len(parts) > 3 else "0"
                    try:
                        if lat_str != "NO_FIX":
                            last_gps_sats = int(sats_str)
                            last_gps_lat  = float(lat_str)
                            last_gps_lng  = float(lng_str)
                    except (ValueError, IndexError):
                        pass
                    ws_msg = f"GPS_STATS:{sats_str},{lat_str},{lng_str}"
                    print(f"[GPS] Ricevuto: raw='{msg}' → ws='{ws_msg}'")
                # I messaggi STATUS:* vengono passati direttamente al browser
                
                await websocket.send(ws_msg)
        except websockets.exceptions.ConnectionClosed:
            pass
        except Exception as e:
            print(f"[WS] Errore invio: {e}")

    # Task per ricevere comandi
    async def receive_commands_task():
        global imu_zero_roll, imu_zero_pitch, imu_zero_yaw
        global pc_csv_recording, pc_csv_file, pc_csv_filename
        global pc_csv_start_time, pc_csv_session_num, pc_csv_row_count
        try:
            async for message in websocket:

                # ── IMPOSTA ZERO IMU ─────────────────────────────────────────
                if message == "SET_ZERO":
                    imu_zero_roll  = last_imu_roll
                    imu_zero_pitch = last_imu_pitch
                    imu_zero_yaw   = last_imu_yaw
                    print(f"[ZERO] Punto zero impostato: "
                          f"roll={imu_zero_roll:.2f}\u00b0  pitch={imu_zero_pitch:.2f}\u00b0  yaw={imu_zero_yaw:.2f}\u00b0")
                    await websocket.send(
                        f"STATUS:ZERO_SET:{imu_zero_roll:.2f},{imu_zero_pitch:.2f},{imu_zero_yaw:.2f}"
                    )

                # ── AVVIA REGISTRAZIONE ───────────────────────────────────────
                elif message == "START_RECORDING":
                    if not pc_csv_recording:
                        os.makedirs("sessions", exist_ok=True)
                        now = datetime.now()
                        # Formato: session_HHMMSSDDMMYYYY.csv
                        basename = f"session_{now.strftime('%H%M%S%d%m%Y')}"
                        pc_csv_start_time = time.time()
                        start_iso = now.isoformat()
                        pc_csv_filename = f"sessions/{basename}.csv"
                        pc_csv_file = open(pc_csv_filename, "w")
                        # Intestazione con metadati
                        pc_csv_file.write(f"# Sessione: {basename}.csv\n")
                        pc_csv_file.write(f"# Inizio: {start_iso}\n")
                        pc_csv_file.write(
                            f"# Zero IMU: roll={imu_zero_roll:.2f}, pitch={imu_zero_pitch:.2f}, yaw={imu_zero_yaw:.2f}\n"
                        )
                        pc_csv_file.write(
                            "timestamp_iso,elapsed_s,roll,pitch,yaw,gps_lat,gps_lng,gps_sats\n"
                        )
                        pc_csv_row_count = 0
                        pc_csv_recording = True
                        print(f"[REC-PC] CSV avviato: {pc_csv_filename}")
                        await websocket.send(
                            f"STATUS:PC_REC_STARTED:{basename}|{start_iso}"
                        )
                    # SD disabilitata: registrazione solo su PC

                # ── FERMA REGISTRAZIONE ───────────────────────────────────────
                elif message == "STOP_RECORDING":
                    if pc_csv_recording:
                        end_iso = datetime.now().isoformat()
                        pc_csv_recording = False
                        basename = os.path.basename(pc_csv_filename).replace('.csv', '')
                        if pc_csv_file:
                            pc_csv_file.write(f"# Fine: {end_iso}\n")
                            pc_csv_file.write(f"# Righe totali: {pc_csv_row_count}\n")
                            pc_csv_file.flush()
                            pc_csv_file.close()
                            pc_csv_file = None
                        duration = time.time() - pc_csv_start_time
                        print(f"[REC-PC] CSV completato: {pc_csv_filename} "
                              f"({pc_csv_row_count} righe, {duration:.1f}s)")
                        await websocket.send(
                            f"STATUS:PC_REC_STOPPED:{basename}|{end_iso}|{pc_csv_row_count}"
                        )
                    # SD disabilitata: nessun comando all'ESP32

                elif message == "LIST_SESSIONS":
                    if not os.path.exists("sessions"):
                        os.makedirs("sessions")
                    sessions = [
                        f for f in os.listdir("sessions")
                        if f.endswith(".csv")
                    ]
                    await websocket.send(f"SESSIONS:{json.dumps(sessions)}")
                
                # --- NEW SD FILE MANAGEMENT COMMANDS ---
                elif message == "LS_SD":
                    send_command_to_esp32("LS")
                elif message == "INIT_SD":
                    send_command_to_esp32("INIT_SD")
                elif message.startswith("DEL_SD:"):
                    filename = message.split(":")[1]
                    send_command_to_esp32(f"DEL:{filename}")
                elif message.startswith("REN_SD:"):
                    params = message.split(":")[1] # "oldName,newName"
                    send_command_to_esp32(f"REN:{params}")
                elif message.startswith("DWN_SD:"):
                    filename = message.split(":")[1]
                    if esp32_addr:
                        esp_ip = esp32_addr[0]
                        asyncio.create_task(download_file_from_esp32(esp_ip, filename))
                    else:
                        await websocket.send(f"STATUS:SD_DWN_ERROR:{filename}|ESP32_NOT_FOUND")
        except websockets.exceptions.ConnectionClosed:
            pass
        except Exception as e:
            print(f"[WS] Errore ricezione: {e}")

    # Eseguiamo i task. Quando uno dei due termina (disconnessione), l'altro viene cancellato.
    done, pending = await asyncio.wait(
        [asyncio.create_task(send_data_task()), asyncio.create_task(receive_commands_task())],
        return_when=asyncio.FIRST_COMPLETED,
    )
    for task in pending:
        task.cancel()
    print(f"[WS] Visualizer disconnesso ({websocket.remote_address})")

async def main():
    global message_queue, asyncio_loop
    
    asyncio_loop = asyncio.get_running_loop()
    message_queue = asyncio.Queue()

    threading.Thread(target=run_http_server, daemon=True).start()
    threading.Thread(target=udp_listener, daemon=True).start()
    # Pre-scarica le tile della mappa in background (non blocca l'avvio)
    if os.environ.get("SFT_PREFETCH_MAPS") == "1":
        threading.Thread(target=prefetch_map_tiles, daemon=True).start()
    
    print(f"[WS] WebSocket Bridge pronto su porta {WS_PORT}")
    
    try:
        # Usiamo 0.0.0.0 per ascoltare su tutte le interfacce
        async with websockets.serve(ws_handler, "0.0.0.0", WS_PORT):
            await asyncio.Future()  # Run forever
    except Exception as e:
        print(f"Errore di sistema: {e}")

if __name__ == "__main__":
    print("=========================================")
    print("   SFT WIRELESS TELEMETRY BRIDGE V2.0    ")
    print("   con supporto registrazione MicroSD     ")
    print("=========================================")
    print("Assicurati che PC ed ESP32 siano connessi")
    print("alla stessa rete Wi-Fi.")
    print("-----------------------------------------")
    
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nArresto del sistema richiesto dall'utente...")
        sys.exit(0)
