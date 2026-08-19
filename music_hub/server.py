import os
import sys
import time
import threading
import subprocess
import unicodedata
from flask import Flask, send_from_directory

app = Flask(__name__)
MUSIC_DIR = "music"
os.makedirs(MUSIC_DIR, exist_ok=True)

PLAYLIST_URL = "https://open.spotify.com/playlist/3fvPuPeH7CD1RAd2hGtQkB?si=fce3451d03194351"

# Dictionary to remember the real Windows filenames
file_map = {}

def auto_sync_daemon():
    while True:
        print("\n[DAEMON] Waking up. Checking playlist for new tracks...")
        try:
            subprocess.run([
                sys.executable, "-m", "spotdl", "sync", PLAYLIST_URL, 
                "--save-file", "playlist.spotdl",
                "--audio", "youtube"
            ], cwd=MUSIC_DIR, check=True)
            print("[DAEMON] Playlist sync complete! All files up to date.")
        except Exception as e:
            print(f"[DAEMON] Sync error: {e}")
            
        print("[DAEMON] Sleeping for 1 hour...")
        time.sleep(3600)

@app.route('/list')
def list_files():
    global file_map
    file_map.clear()
    files = []
    
    for f in os.listdir(MUSIC_DIR):
        if f.endswith('.mp3'):
            # Strip Polish/Spanish accents (ś -> s, ł -> l) for ESP32 and TFT compatibility
            clean_name = unicodedata.normalize('NFKD', f).encode('ASCII', 'ignore').decode('ASCII')
            file_map[clean_name] = f # Remember the translation
            files.append(clean_name)
            
    return '\n'.join(files)

@app.route('/music/<path:filename>')
def download_file(filename):
    # Translate the clean ESP32 request back to the real Windows filename
    real_filename = file_map.get(filename, filename)
    return send_from_directory(MUSIC_DIR, real_filename)

if __name__ == '__main__':
    threading.Thread(target=auto_sync_daemon, daemon=True).start()
    
    print("========================================")
    print(" ESP32 Auto-Sync Hub is Running!        ")
    print("========================================")
    app.run(host='0.0.0.0', port=5000)