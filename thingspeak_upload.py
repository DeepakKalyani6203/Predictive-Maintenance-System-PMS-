
import csv
import os
import time
from pathlib import Path
import requests

BASE = Path.home() / "pms_c"
ADXL_FILE = BASE / "logs" / "adxl345.csv"
TEMP_FILE = BASE / "logs" / "ds18b20.csv"
MIC_FILE = BASE / "logs" / "audio" / "ics43434_levels.csv"

API_KEY = "2J6X4WI1HCSUDWTB"
URL = "https://api.thingspeak.com/update"
INTERVAL = 16


def latest_temperature():
    latest = None

    try:
        with TEMP_FILE.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.lower().startswith("temperature"):
                    continue

                # Supports: 25.937, 26-10-23 11:38:03
                # Also supports older rows containing a pipe separator.
                value = line.split(",", 1)[0].split("|", 1)[0].strip()

                try:
                    latest = float(value)
                except ValueError:
                    continue
    except OSError as e:
        print(f"Temperature file error: {e}")

    return latest


def latest_microphone_levels():
    latest = None

    try:
        with MIC_FILE.open("r", encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)

            for row in reader:
                try:
                    rms = float(row["rms"])
                    peak = float(row["peak"])
                    if rms >= 0 and peak >= 0:
                        latest = (rms, peak)
                except (KeyError, TypeError, ValueError):
                    continue

    except OSError as e:
        print(f"Microphone file error: {e}")

    return latest


def upload_values(x, y, z, temperature, mic):
    payload = {
        "api_key": API_KEY,
        "field1": round(x, 4),
        "field2": round(y, 4),
        "field3": round(z, 4),
    }

    if temperature is not None:
        payload["field4"] = round(temperature, 3)
    else:
        print("No valid temperature reading yet; skipping upload.")
        return

    if mic is not None:
        rms, peak = mic
        payload["field5"] = round(rms, 8)
        payload["field6"] = round(peak, 8)
    else:
        print("No valid microphone RMS/peak reading yet; uploading other sensors.")

    try:
        response = requests.post(URL, data=payload, timeout=10)
        response.raise_for_status()
        result = response.text.strip()

        if result.isdigit() and int(result) > 0:
            mic_text = (
                f" | Mic RMS={mic[0]:.8f}, Peak={mic[1]:.8f}"
                if mic is not None else " | Mic data unavailable"
            )

            print(
                f"Uploaded entry {result} | "
                f"X={x:.3f}, Y={y:.3f}, Z={z:.3f} m/s² | "
                f"Temperature={temperature:.3f} °C"
                f"{mic_text}"
            )
        else:
            print(f"ThingSpeak rejected the update: {result}")

    except requests.RequestException as e:
        print(f"Upload failed: {e}")


def read_acceleration(line):
    try:
        row = next(csv.reader([line]))
        if len(row) < 4:
            return None

        # CSV columns: Timestamp, X Axis, Y Axis, Z Axis
        return tuple(float(row[i].strip()) for i in (1, 2, 3))
    except (ValueError, StopIteration):
        return None


def main():
    if not API_KEY:
        print("API key missing. Export THINGSPEAK_WRITE_API_KEY first.")
        return

    if not ADXL_FILE.exists() or not TEMP_FILE.exists():
        print("Missing sensor CSV file. Check the paths:")
        print(ADXL_FILE)
        print(TEMP_FILE)
        return

    temperature = latest_temperature()
    mic = latest_microphone_levels()

    print(f"Uploader started. Initial temperature: {temperature}")
    print(f"Initial microphone reading: {mic}")
    print("Waiting for new ADXL345 samples; press Ctrl+C to stop.")

    # Start at the end so historical acceleration samples are not uploaded.
    with ADXL_FILE.open("r", encoding="utf-8", newline="") as adxl:
        adxl.seek(0, os.SEEK_END)
        xs, ys, zs = [], [], []
        last_upload = time.monotonic()

        while True:
            while True:
                line = adxl.readline()
                if not line:
                    break

                values = read_acceleration(line)
                if values:
                    x, y, z = values
                    xs.append(x)
                    ys.append(y)
                    zs.append(z)

            # Refresh temperature and microphone readings each cycle.
            new_temperature = latest_temperature()
            if new_temperature is not None:
                temperature = new_temperature

            new_mic = latest_microphone_levels()
            if new_mic is not None:
                mic = new_mic

            now = time.monotonic()
            if now - last_upload >= INTERVAL:
                if xs and temperature is not None:
                    avg_x = sum(xs) / len(xs)
                    avg_y = sum(ys) / len(ys)
                    avg_z = sum(zs) / len(zs)

                    print(f"Read {len(xs)} new samples.")
                    upload_values(avg_x, avg_y, avg_z, temperature, mic)

                    xs.clear()
                    ys.clear()
                    zs.clear()
                elif not xs:
                    print("No new ADXL345 samples in this interval.")
                else:
                    print("Waiting for a valid temperature reading.")

                last_upload = now

            time.sleep(0.2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nUploader stopped.")
