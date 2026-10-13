import serial
import glob
import time
import sys

try:
    port = glob.glob('/dev/cu.usbmodem*')[0]
    print(f"Connecting to {port}...")
    s = serial.Serial(port, 115200, timeout=0.1)
    
    s.setDTR(False)
    s.setRTS(False)
    time.sleep(0.5)
    s.setDTR(True)
    s.setRTS(True)
    time.sleep(0.5)
    s.reset_input_buffer()
    
    print("Reading for 45 seconds (brute force takes time)...")
    end_time = time.time() + 45
    
    while time.time() < end_time:
        raw_line = s.readline().decode('utf-8', errors='ignore').strip()
        if raw_line:
            print(raw_line)
            if "SCAN COMPLETE" in raw_line or "WE FOUND IT!" in raw_line:
                time.sleep(1) # Read trailing lines
                while s.in_waiting:
                    print(s.readline().decode('utf-8', errors='ignore').strip())
                break
                
except Exception as e:
    print(f"Error: {e}")
