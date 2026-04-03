"""
Bridge server: ESP32 (Pixel) → Ollama + Blender MCP + AgentMail
With: Conversation Memory, Mood Evolution, GitHub/Weather Notifications, Image Serving
"""

import json
import socket
import re
import time
import struct
import threading
import base64
import os
import requests
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime, timezone
from PIL import Image
import io

OLLAMA_URL = "http://localhost:11434/api/generate"
CHAT_MODEL = "gemma4"
CODE_MODEL = "gemma4"
IMAGE_MODEL = "x/z-image-turbo"
IMAGE_OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "generated_images")
BLENDER_HOST = "localhost"
BLENDER_PORT = 9876

# AgentMail config
AGENTMAIL_API_KEY = "am_us_710309f9faf50b2aa4e82ec3b7fedd9c8b889e2bd8bddee444c71ab280101757"
AGENTMAIL_INBOX = "loveme@agentmail.to"
AGENTMAIL_BASE = "https://api.agentmail.to/v0"
EMAIL_POLL_INTERVAL = 30  # seconds
AGENTMAIL_INBOX_ENCODED = AGENTMAIL_INBOX.replace("@", "%40")
AGENTMAIL_HEADERS = {
    "Authorization": f"Bearer {AGENTMAIL_API_KEY}",
    "Content-Type": "application/json"
}

# Notification Hub config (optional, via environment variables)
GITHUB_TOKEN = os.environ.get("GITHUB_TOKEN", "")
WEATHER_API_KEY = os.environ.get("WEATHER_API_KEY", "")
WEATHER_CITY = os.environ.get("WEATHER_CITY", "New York")
GITHUB_POLL_INTERVAL = 60  # seconds
WEATHER_POLL_INTERVAL = 900  # 15 minutes

# Conversation Memory config
MEMORY_MAX_EXCHANGES = 10

# Mood Evolution config
MOOD_STATES = ["happy", "sad", "thinking", "talking", "surprised"]
MOOD_INTERACTION_THRESHOLD = 300   # 5 min in seconds — frequent interaction
MOOD_LONELY_THRESHOLD = 1800       # 30 min in seconds — long gap
MOOD_PENDING_EMAIL_ANXIETY = 3     # pending emails that trigger "surprised"

# ---- Conversation Memory ----
conversation_history = []  # List of {"role": "user"/"assistant", "message": "..."}
memory_lock = threading.Lock()


def add_to_memory(role, message):
    """Add an exchange to rolling conversation history, trimming to last N exchanges."""
    with memory_lock:
        conversation_history.append({"role": role, "message": message})
        # Each exchange is user+assistant, so keep 2*N entries for N exchanges
        max_entries = MEMORY_MAX_EXCHANGES * 2
        while len(conversation_history) > max_entries:
            conversation_history.pop(0)
        print(f"[Memory] {role}: {message[:60]}... ({len(conversation_history)} entries)")


def build_memory_context():
    """Build a string of recent conversation history for inclusion in prompts."""
    with memory_lock:
        if not conversation_history:
            return ""
        lines = []
        for entry in conversation_history:
            role_label = "User" if entry["role"] == "user" else "Pixel"
            lines.append(f"{role_label}: {entry['message']}")
        context = "\n".join(lines)
        return f"\nRecent conversation:\n{context}\n"


# ---- Mood Evolution System ----
mood_state = {
    "current_mood": "happy",
    "last_interaction_time": time.time(),
    "interaction_count": 0,
    "last_mood_update": time.time(),
}
mood_lock = threading.Lock()


def update_mood_on_interaction():
    """Update mood state when a user interaction occurs."""
    with mood_lock:
        now = time.time()
        gap = now - mood_state["last_interaction_time"]
        mood_state["last_interaction_time"] = now
        mood_state["interaction_count"] += 1

        # Check pending notifications (use len() directly — GIL-safe for reads)
        pending_count = len(email_notifications) + len(general_notifications)
        if pending_count > MOOD_PENDING_EMAIL_ANXIETY:
            mood_state["current_mood"] = "surprised"
            print(f"[Mood] → surprised (too many pending notifications: {pending_count})")
        elif gap < MOOD_INTERACTION_THRESHOLD:
            # Frequent interactions → happy
            mood_state["current_mood"] = "happy"
            print(f"[Mood] → happy (frequent interaction, gap={gap:.0f}s)")
        elif gap > MOOD_LONELY_THRESHOLD:
            # Long gap → sad
            mood_state["current_mood"] = "sad"
            print(f"[Mood] → sad (long gap: {gap:.0f}s)")
        else:
            # Normal interaction
            mood_state["current_mood"] = "talking"
            print(f"[Mood] → talking (normal interaction)")

        mood_state["last_mood_update"] = now


def set_mood(mood):
    """Directly set the mood (e.g., 'thinking' during generation)."""
    with mood_lock:
        if mood in MOOD_STATES:
            mood_state["current_mood"] = mood
            mood_state["last_mood_update"] = time.time()
            print(f"[Mood] → {mood} (set directly)")


def get_mood():
    """Get current mood, applying time-based decay."""
    with mood_lock:
        now = time.time()
        gap = now - mood_state["last_interaction_time"]

        # If no interaction for a long time, drift toward sad
        if gap > MOOD_LONELY_THRESHOLD and mood_state["current_mood"] != "sad":
            mood_state["current_mood"] = "sad"
            mood_state["last_mood_update"] = now
            print(f"[Mood] → sad (decay, no interaction for {gap:.0f}s)")

        # Check pending notifications (use len() directly — GIL-safe for reads)
        pending_count = len(email_notifications) + len(general_notifications)
        if pending_count > MOOD_PENDING_EMAIL_ANXIETY:
            mood_state["current_mood"] = "surprised"

        return mood_state["current_mood"]


# ---- Notifications Queue (thread-safe) ----
email_notifications = []
general_notifications = []
notifications_lock = threading.Lock()
last_email_check = None


def pending_notification_count():
    with notifications_lock:
        return len(email_notifications) + len(general_notifications)


def poll_emails():
    """Background thread that polls AgentMail for new messages."""
    global last_email_check
    last_seen_id = None

    print(f"[Email] Polling {AGENTMAIL_INBOX} every {EMAIL_POLL_INTERVAL}s")

    while True:
        try:
            url = f"{AGENTMAIL_BASE}/inboxes/{AGENTMAIL_INBOX_ENCODED}/messages?limit=5&order=desc"
            resp = requests.get(url, headers=AGENTMAIL_HEADERS, timeout=15)

            if resp.status_code == 200:
                data = resp.json()
                messages = data.get("messages", data.get("data", []))

                if messages and isinstance(messages, list):
                    newest = messages[0]
                    newest_id = newest.get("message_id", newest.get("id", ""))

                    if last_seen_id is None:
                        # First poll — just record the latest, don't notify
                        last_seen_id = newest_id
                        print(f"[Email] Initial sync. Latest: {newest_id[:20]}...")
                    elif newest_id != last_seen_id:
                        # New email(s)!
                        for msg in messages:
                            msg_id = msg.get("message_id", msg.get("id", ""))
                            if msg_id == last_seen_id:
                                break

                            sender = msg.get("from", msg.get("sender", "unknown"))
                            if isinstance(sender, dict):
                                sender = sender.get("email", sender.get("address", "unknown"))
                            subject = msg.get("subject", "No subject")

                            # Get email body (AgentMail uses 'preview' field)
                            body_text = msg.get("preview", msg.get("body", msg.get("text", "")))
                            if isinstance(body_text, dict):
                                body_text = body_text.get("text", body_text.get("plain", ""))
                            body_text = str(body_text)[:500]

                            # AI summary
                            summary = ""
                            if body_text:
                                try:
                                    sr = requests.post(OLLAMA_URL, json={
                                        "model": CHAT_MODEL,
                                        "prompt": f"Summarize this email in 1 short sentence:\nFrom: {sender}\nSubject: {subject}\n\n{body_text}",
                                        "system": "You summarize emails in exactly 1 short sentence. Be concise.",
                                        "stream": False,
                                        "options": {"num_predict": 40, "temperature": 0.3}
                                    }, timeout=20)
                                    if sr.status_code == 200:
                                        summary = sr.json().get("response", "").strip()[:100]
                                except:
                                    summary = body_text[:80]

                            # Clean text for ESP32 (no newlines, keep short)
                            clean_summary = summary.replace("\n", " ").replace("\r", "").strip()[:80]
                            clean_body = body_text.split("\n")[0].strip()[:80]  # First line only
                            clean_from = str(sender).split("<")[0].strip()[:30]  # Name only

                            notification = {
                                "type": "email",
                                "from": clean_from,
                                "subject": str(subject)[:50],
                                "summary": clean_summary,
                                "body": clean_body
                            }
                            with notifications_lock:
                                email_notifications.append(notification)
                            print(f"[Email] New: {sender} - {subject}")
                            print(f"[Email] Summary: {summary}")
                            print(f"[Email] Body: {body_text[:100]}...")

                        last_seen_id = newest_id

                last_email_check = datetime.now(timezone.utc).isoformat()
            else:
                print(f"[Email] Poll error: {resp.status_code}")

        except Exception as e:
            print(f"[Email] Error: {e}")

        time.sleep(EMAIL_POLL_INTERVAL)


# ---- GitHub Notifications Polling ----
def poll_github():
    """Background thread that polls GitHub for unread notifications."""
    if not GITHUB_TOKEN:
        return

    headers = {
        "Authorization": f"token {GITHUB_TOKEN}",
        "Accept": "application/vnd.github.v3+json"
    }
    last_seen_ids = set()

    print(f"[GitHub] Polling notifications every {GITHUB_POLL_INTERVAL}s")

    while True:
        try:
            resp = requests.get(
                "https://api.github.com/notifications",
                headers=headers,
                timeout=15
            )
            if resp.status_code == 200:
                notifications = resp.json()
                for notif in notifications:
                    notif_id = notif.get("id", "")
                    if notif_id and notif_id not in last_seen_ids:
                        last_seen_ids.add(notif_id)
                        repo = notif.get("repository", {}).get("full_name", "unknown")
                        reason = notif.get("reason", "unknown")
                        title = notif.get("subject", {}).get("title", "No title")
                        notif_type = notif.get("subject", {}).get("type", "")

                        clean_title = str(title)[:60].replace("\n", " ")
                        clean_repo = str(repo)[:40]

                        with notifications_lock:
                            general_notifications.append({
                                "type": "github",
                                "repo": clean_repo,
                                "reason": reason,
                                "title": clean_title,
                                "github_type": notif_type,
                                "summary": f"{clean_repo}: {clean_title}"
                            })
                        print(f"[GitHub] New: {clean_repo} - {clean_title} ({reason})")

                # Keep only last 50 seen IDs to prevent unbounded growth
                if len(last_seen_ids) > 200:
                    last_seen_ids.clear()
            elif resp.status_code == 401:
                print("[GitHub] Invalid token. Stopping poll.")
                return
            else:
                print(f"[GitHub] Poll error: {resp.status_code}")

        except Exception as e:
            print(f"[GitHub] Error: {e}")

        time.sleep(GITHUB_POLL_INTERVAL)


# ---- Weather Polling ----
last_weather = None


def poll_weather():
    """Background thread that polls OpenWeatherMap for weather updates."""
    global last_weather
    if not WEATHER_API_KEY:
        return

    print(f"[Weather] Polling for {WEATHER_CITY} every {WEATHER_POLL_INTERVAL}s")

    while True:
        try:
            url = (
                f"https://api.openweathermap.org/data/2.5/weather"
                f"?q={WEATHER_CITY}&appid={WEATHER_API_KEY}&units=metric"
            )
            resp = requests.get(url, timeout=15)
            if resp.status_code == 200:
                data = resp.json()
                weather_main = data.get("weather", [{}])[0].get("main", "Unknown")
                weather_desc = data.get("weather", [{}])[0].get("description", "")
                temp = data.get("main", {}).get("temp", 0)
                feels_like = data.get("main", {}).get("feels_like", 0)

                weather_summary = f"{WEATHER_CITY}: {weather_desc}, {temp:.0f}C (feels {feels_like:.0f}C)"

                # Only notify on weather change
                if last_weather is None or last_weather != weather_main:
                    with notifications_lock:
                        general_notifications.append({
                            "type": "weather",
                            "city": WEATHER_CITY,
                            "condition": weather_main,
                            "description": weather_desc,
                            "temp": round(temp, 1),
                            "feels_like": round(feels_like, 1),
                            "summary": weather_summary
                        })
                    print(f"[Weather] Update: {weather_summary}")
                    last_weather = weather_main
                else:
                    print(f"[Weather] No change: {weather_summary}")
            elif resp.status_code == 401:
                print("[Weather] Invalid API key. Stopping poll.")
                return
            else:
                print(f"[Weather] Poll error: {resp.status_code}")

        except Exception as e:
            print(f"[Weather] Error: {e}")

        time.sleep(WEATHER_POLL_INTERVAL)


# ---- Blender MCP ----
class BlenderBridge:
    def __init__(self):
        self.connected = False

    def connect(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(3)
            s.connect((BLENDER_HOST, BLENDER_PORT))
            s.close()
            self.connected = True
            return True
        except:
            self.connected = False
            return False

    def send_command(self, command, params=None):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect((BLENDER_HOST, BLENDER_PORT))
            msg = {"type": command}
            if params:
                msg["params"] = params
            s.sendall((json.dumps(msg) + "\n").encode())
            response = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                response += chunk
                try:
                    result = json.loads(response.decode())
                    s.close()
                    return result
                except json.JSONDecodeError:
                    continue
            s.close()
            return {"error": "No response"}
        except Exception as e:
            return {"error": str(e)}

    def execute_code(self, code):
        return self.send_command("execute_code", {"code": code})

blender = BlenderBridge()

# ---- Image Generation ----
os.makedirs(IMAGE_OUTPUT_DIR, exist_ok=True)

# Track the most recently generated image
latest_image = {"path": None, "filename": None, "timestamp": 0}
latest_image_lock = threading.Lock()


def is_image_request(prompt):
    lower = prompt.lower()
    triggers = ["draw", "paint", "sketch", "image of", "picture of", "photo of",
                "generate image", "generate a image", "generate an image",
                "create image", "create a image", "create an image",
                "make image", "make a image", "make an image",
                "show me", "illustrate", "render image", "visualize"]
    return any(t in lower for t in triggers)

def generate_image(prompt):
    try:
        print(f"[ImageGen] Generating: {prompt}")
        resp = requests.post(OLLAMA_URL, json={
            "model": IMAGE_MODEL,
            "prompt": prompt,
            "stream": False
        }, timeout=300)
        if resp.status_code == 200:
            data = resp.json()
            image = data.get("image", data.get("images", [None])[0] if data.get("images") else None)
            if image:
                img_data = base64.b64decode(image)
                timestamp = int(time.time())
                filename = f"pixel_{timestamp}.png"
                filepath = os.path.join(IMAGE_OUTPUT_DIR, filename)
                with open(filepath, "wb") as f:
                    f.write(img_data)
                print(f"[ImageGen] Saved: {filepath}")

                # Update latest image tracker
                with latest_image_lock:
                    latest_image["path"] = filepath
                    latest_image["filename"] = filename
                    latest_image["timestamp"] = timestamp

                return {"path": filepath, "filename": filename, "base64": image[:100] + "..."}
            else:
                print(f"[ImageGen] No images in response. Keys: {list(data.keys())}")
                return None
        else:
            print(f"[ImageGen] Error: {resp.status_code}")
    except Exception as e:
        print(f"[ImageGen] Error: {e}")
    return None


_thumbnail_cache = {"path": None, "data": None}

def get_latest_image_thumbnail():
    """
    Returns the latest generated image resized to 320x170 in RGB565 format
    (big-endian, 2 bytes per pixel) for direct ESP32 TFT display.
    Caches the result until a new image is generated.
    """
    with latest_image_lock:
        filepath = latest_image.get("path")

    if not filepath or not os.path.exists(filepath):
        return None

    # Return cached thumbnail if same image
    if _thumbnail_cache["path"] == filepath and _thumbnail_cache["data"]:
        return _thumbnail_cache["data"]

    try:
        img = Image.open(filepath).convert("RGB")
        img = img.resize((320, 170), Image.LANCZOS)

        # Convert to RGB565 big-endian using struct for speed
        pixels = img.load()
        width, height = img.size
        buf = bytearray(width * height * 2)
        idx = 0
        for y in range(height):
            for x in range(width):
                r, g, b = pixels[x, y]
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                struct.pack_into(">H", buf, idx, rgb565)
                idx += 2

        result = bytes(buf)
        _thumbnail_cache["path"] = filepath
        _thumbnail_cache["data"] = result
        return result
    except Exception as e:
        print(f"[ImageGen] Thumbnail error: {e}")
        return None


# ---- Blender Intent ----
def is_blender_request(prompt):
    lower = prompt.lower()
    if "blender" in lower:
        return True
    verbs = ["create", "make", "add", "build", "generate", "put", "place", "delete", "remove", "clear"]
    objects = ["cube", "sphere", "cylinder", "cone", "torus", "monkey", "suzanne",
               "plane", "circle", "mesh", "object", "light", "camera", "material",
               "donut", "snowman", "house", "tree", "car", "box", "ball", "ring"]
    return any(v in lower for v in verbs) and any(o in lower for o in objects)

def generate_blender_code(prompt):
    try:
        resp = requests.post(OLLAMA_URL, json={
            "model": CODE_MODEL,
            "prompt": f'Write ONLY Blender Python code (bpy) for: "{prompt}". No explanation, no markdown. Start with import bpy.',
            "stream": False,
            "options": {"num_predict": 500, "temperature": 0.3}
        }, timeout=30)
        if resp.status_code == 200:
            code = resp.json().get("response", "").strip()
            code = re.sub(r'^```\w*\n?', '', code)
            code = re.sub(r'\n?```$', '', code)
            code = code.strip()
            if not code.startswith("import"):
                code = "import bpy\n" + code
            return code
    except Exception as e:
        print(f"[CodeGen] Error: {e}")
    return None

def get_chat_response(prompt):
    """Get a chat response from Ollama, including conversation memory in the prompt."""
    memory_context = build_memory_context()

    system_prompt = "You are Pixel, a cute AI desk pet with glowing amber eyes. Keep responses to 1 short sentence."
    if memory_context:
        system_prompt += f"\n{memory_context}"

    try:
        resp = requests.post(OLLAMA_URL, json={
            "model": CHAT_MODEL,
            "prompt": prompt,
            "system": system_prompt,
            "stream": False,
            "options": {"num_predict": 40, "temperature": 0.8}
        }, timeout=20)
        if resp.status_code == 200:
            return resp.json().get("response", "Done!").strip()
    except:
        pass
    return "Done!"


# ---- HTTP Server ----
class BridgeHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()

        # Email reply endpoint
        if self.path == "/email-reply":
            self._handle_email_reply(body)
            return

        try:
            data = json.loads(body)
            prompt = data.get("prompt", "")
        except:
            prompt = body

        print(f"\n[Pixel] > {prompt}")

        # Record user message in memory
        add_to_memory("user", prompt)

        # Update mood on interaction
        update_mood_on_interaction()

        if is_image_request(prompt):
            print("[Bridge] Image generation request detected!")
            set_mood("thinking")
            display = "Let me draw that for you..."
            result = generate_image(prompt)
            if result:
                display = get_chat_response(f"I just drew an image of: {prompt}. Respond happily in 1 sentence.")[:120]
                add_to_memory("assistant", display)
                current_mood = get_mood()
                print(f"[Pixel] < {display}")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({
                    "response": display,
                    "mood": current_mood,
                    "image": result.get("filename"),
                    "image_path": result.get("path")
                }).encode())
                return
            else:
                display = "Sorry, I couldn't generate that image..."
        elif is_blender_request(prompt):
            print("[Bridge] Blender request detected!")
            set_mood("thinking")
            code = generate_blender_code(prompt)
            if code:
                print(f"[Bridge] Code:\n{code[:200]}")
                result = blender.execute_code(code)
                print(f"[Blender] Result: {result}")
                if result and "error" not in str(result).lower():
                    display = get_chat_response(f"I just did this in Blender: {prompt}. Respond happily.")[:120]
                else:
                    display = f"Oops: {str(result.get('error','?'))[:60]}"
            else:
                display = "Couldn't generate the code..."
        else:
            set_mood("thinking")
            display = get_chat_response(prompt)
            if len(display) > 120:
                display = display[:117] + "..."

        # Record assistant response in memory
        add_to_memory("assistant", display)
        current_mood = get_mood()

        print(f"[Pixel] < {display}")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({
            "response": display,
            "mood": current_mood
        }).encode())

    def do_GET(self):
        path = self.path

        # GET /mood — return current mood state
        if path == "/mood":
            current_mood = get_mood()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "mood": current_mood,
                "last_interaction": mood_state.get("last_interaction_time", 0),
                "interaction_count": mood_state.get("interaction_count", 0)
            }).encode())
            return

        # GET /images/latest/thumbnail — RGB565 raw bytes for ESP32 display
        if path == "/images/latest/thumbnail":
            thumbnail_data = get_latest_image_thumbnail()
            if thumbnail_data:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(thumbnail_data)))
                self.end_headers()
                self.wfile.write(thumbnail_data)
            else:
                self.send_response(404)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "No image available"}).encode())
            return

        # GET /images/latest — metadata of most recent generated image
        if path == "/images/latest":
            with latest_image_lock:
                img_info = dict(latest_image)
            if img_info.get("path") and os.path.exists(img_info["path"]):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({
                    "filename": img_info["filename"],
                    "path": img_info["path"],
                    "timestamp": img_info["timestamp"],
                    "url": f"/images/{img_info['filename']}"
                }).encode())
            else:
                self.send_response(404)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "No image generated yet"}).encode())
            return

        # Serve generated images
        if path.startswith("/images/"):
            filename = path.split("/images/")[-1]
            filepath = os.path.join(IMAGE_OUTPUT_DIR, filename)
            if os.path.exists(filepath):
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.end_headers()
                with open(filepath, "rb") as f:
                    self.wfile.write(f.read())
            else:
                self.send_response(404)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": "Image not found"}).encode())
            return

        if path == "/check-email":
            # Fetch latest emails and return a summary
            try:
                r = requests.get(
                    f"{AGENTMAIL_BASE}/inboxes/{AGENTMAIL_INBOX_ENCODED}/messages?limit=5&order=desc",
                    headers=AGENTMAIL_HEADERS, timeout=15
                )
                msgs = r.json().get("messages", [])
                email_list = []
                for m in msgs[:5]:
                    sender = m.get("from", "?")
                    if isinstance(sender, dict):
                        sender = sender.get("email", "?")
                    sender = str(sender).split("<")[0].strip()[:25]
                    subj = str(m.get("subject", "No subject"))[:40]
                    preview = str(m.get("preview", ""))[:50].replace("\n", " ")
                    email_list.append({"from": sender, "subject": subj, "preview": preview})

                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({
                    "count": len(email_list),
                    "emails": email_list
                }).encode())
            except Exception as e:
                self.send_response(500)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode())
            return

        if path.startswith("/notifications"):
            # Return pending email + general notifications (GitHub, Weather)
            # Only clear when ESP32 confirms with /notifications?ack=1
            with notifications_lock:
                all_notifs = list(email_notifications) + list(general_notifications)
                ack = "ack=1" in (self.path or "")
                if ack:
                    email_notifications.clear()
                    general_notifications.clear()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "count": len(all_notifs),
                "notifications": all_notifs
            }).encode())
            return

        # Health check
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({
            "status": "ok",
            "mood": get_mood(),
            "blender": "connected" if blender.connected else "disconnected",
            "email": AGENTMAIL_INBOX,
            "pending_notifications": pending_notification_count(),
            "last_email_check": last_email_check,
            "memory_entries": len(conversation_history),
            "github_enabled": bool(GITHUB_TOKEN),
            "weather_enabled": bool(WEATHER_API_KEY)
        }).encode())

    def _handle_email_reply(self, body):
        """Generate an AI reply to an email and send it back via AgentMail."""
        try:
            data = json.loads(body)
            sender = data.get("from", "someone")
            subject = data.get("subject", "")
            email_body = data.get("body", "")
        except:
            sender, subject, email_body = "someone", "", body

        print(f"\n[Email Reply] From: {sender}")
        print(f"[Email Reply] Subject: {subject}")
        print(f"[Email Reply] Body: {email_body}")

        # Generate reply with AI
        try:
            resp = requests.post(OLLAMA_URL, json={
                "model": CHAT_MODEL,
                "prompt": (
                    f"You received this email:\n"
                    f"From: {sender}\n"
                    f"Subject: {subject}\n"
                    f"Message: {email_body}\n\n"
                    f"Write a helpful, warm reply in 2-3 sentences. Be friendly and answer any questions."
                ),
                "system": (
                    "You are Pixel, an AI desk pet assistant. You reply to emails on behalf of your owner. "
                    "Be warm, helpful, and concise. Sign off as 'Pixel (AI Assistant)'."
                ),
                "stream": False,
                "options": {"num_predict": 100, "temperature": 0.7}
            }, timeout=30)
            if resp.status_code == 200:
                ai_reply = resp.json().get("response", "").strip()
            else:
                ai_reply = "Thanks for your email! I'll let my owner know."
        except:
            ai_reply = "Thanks for your email! I'll let my owner know."

        print(f"[Email Reply] AI says: {ai_reply}")

        # Send reply via AgentMail
        reply_sent = False
        try:
            # Extract email address from sender
            reply_to = sender
            if "<" in reply_to and ">" in reply_to:
                reply_to = reply_to.split("<")[1].split(">")[0]

            send_url = f"{AGENTMAIL_BASE}/inboxes/{AGENTMAIL_INBOX_ENCODED}/messages/send"
            reply_subject = subject if subject.startswith("Re:") else f"Re: {subject}"
            send_data = {
                "to": [reply_to],
                "subject": reply_subject,
                "body": ai_reply
            }
            sr = requests.post(send_url, json=send_data, headers=AGENTMAIL_HEADERS, timeout=15)
            if sr.status_code in (200, 201):
                reply_sent = True
                print(f"[Email Reply] Sent reply to {reply_to}")
            else:
                print(f"[Email Reply] Send failed: {sr.status_code} {sr.text[:100]}")
        except Exception as e:
            print(f"[Email Reply] Send error: {e}")

        # Truncate for display on ESP32
        display_reply = ai_reply[:110]

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({
            "reply": display_reply,
            "sent": reply_sent,
            "mood": get_mood()
        }).encode())

    def log_message(self, format, *args):
        pass


def main():
    print("=" * 50)
    print("  Pixel Bridge Server")
    print("  ESP32 → Ollama + Blender + AgentMail")
    print("  + Memory, Mood, GitHub, Weather, Images")
    print("=" * 50)

    print(f"\n[Memory] Conversation history: max {MEMORY_MAX_EXCHANGES} exchanges")
    print(f"[Mood] Initial mood: {mood_state['current_mood']}")

    print("\n[Blender] Connecting...")
    if blender.connect():
        print("[Blender] Connected!")
    else:
        print("[Blender] Not connected. Will retry on command.")

    # Start email polling thread
    email_thread = threading.Thread(target=poll_emails, daemon=True)
    email_thread.start()
    print(f"[Email] Monitoring {AGENTMAIL_INBOX}")

    # Start GitHub polling thread (only if token is set)
    if GITHUB_TOKEN:
        github_thread = threading.Thread(target=poll_github, daemon=True)
        github_thread.start()
        print(f"[GitHub] Monitoring notifications (token: ...{GITHUB_TOKEN[-4:]})")
    else:
        print("[GitHub] No GITHUB_TOKEN set. GitHub notifications disabled.")

    # Start Weather polling thread (only if API key is set)
    if WEATHER_API_KEY:
        weather_thread = threading.Thread(target=poll_weather, daemon=True)
        weather_thread.start()
        print(f"[Weather] Monitoring weather for {WEATHER_CITY}")
    else:
        print("[Weather] No WEATHER_API_KEY set. Weather updates disabled.")

    port = 8888
    server = HTTPServer(("0.0.0.0", port), BridgeHandler)
    print(f"\n[Server] Listening on http://0.0.0.0:{port}")
    print("[Server] Endpoints:")
    print("  POST /           - Chat with Pixel")
    print("  POST /email-reply - Reply to email")
    print("  GET  /           - Health check")
    print("  GET  /mood       - Current mood state")
    print("  GET  /notifications - Pending notifications (email, github, weather)")
    print("  GET  /check-email - Check inbox")
    print("  GET  /images/<file> - Serve image")
    print("  GET  /images/latest - Latest image metadata")
    print("  GET  /images/latest/thumbnail - RGB565 thumbnail for ESP32")
    print("[Server] Waiting for Pixel...\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Server] Shutting down.")
        server.server_close()

if __name__ == "__main__":
    main()
