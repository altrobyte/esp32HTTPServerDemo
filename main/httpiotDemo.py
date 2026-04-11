import requests
import time

ESP32_IP = "10.162.189.252"  # ESP32 ka IP monitor se copy karo

while True:
    try:
        r = requests.get(f"http://{ESP32_IP}/status", timeout=5)
        print(f"ESP32: {r.json()}")
        requests.post(f"http://{ESP32_IP}/led",
              json={"command": "LED_OFF"})
    except Exception as e:
        print(f"Error: {e}")
    time.sleep(3)