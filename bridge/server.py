"""
Bridge server: ESP32 (Pixel) → Ollama + Blender MCP + AgentMail
"""

import json
import socket
import re
import time
import threading
import requests
from http.server import HTTPServer, BaseHTTPRequestHandler
from datetime import datetime, timezone

OLLAMA_URL = "http://localhost:11434/api/generate"
CHAT_MODEL = "gemma3"
CODE_MODEL = "mistral-small"
BLENDER_HOST = "localhost"
BLENDER_PORT = 9876

# AgentMail config
AGENTMAIL_API_KEY = "am_us_710309f9faf50b2aa4e82ec3b7fedd9c8b889e2bd8bddee444c71ab280101757"
AGENTMAIL_INBOX = "loveme@agentmail.to"
AGENTMAIL_BASE = "https://api.agentmail.to/v0"
EMAIL_POLL_INTERVAL = 30  # seconds

# ---- Email Notifications Queue ----
email_notifications = []  # List of unread notification dicts
last_email_check = None

def poll_emails():
    """Background thread that polls AgentMail for new messages."""
    global last_email_check
    last_seen_id = None

    headers = {
        "Authorization": f"Bearer {AGENTMAIL_API_KEY}",
        "Content-Type": "application/json"
    }
    inbox_encoded = AGENTMAIL_INBOX.replace("@", "%40")

    print(f"[Email] Polling {AGENTMAIL_INBOX} every {EMAIL_POLL_INTERVAL}s")

    while True:
        try:
            url = f"{AGENTMAIL_BASE}/inboxes/{inbox_encoded}/messages?limit=5&order=desc"
            resp = requests.get(url, headers=headers, timeout=15)

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
                                "from": clean_from,
                                "subject": str(subject)[:50],
                                "summary": clean_summary,
                                "body": clean_body
                            }
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
    try:
        resp = requests.post(OLLAMA_URL, json={
            "model": CHAT_MODEL,
            "prompt": prompt,
            "system": "You are Pixel, a cute AI desk pet with glowing amber eyes. Keep responses to 1 short sentence.",
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

        if is_blender_request(prompt):
            print("[Bridge] Blender request detected!")
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
            display = get_chat_response(prompt)
            if len(display) > 120:
                display = display[:117] + "..."

        print(f"[Pixel] < {display}")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"response": display}).encode())

    def do_GET(self):
        path = self.path

        if path == "/check-email":
            # Fetch latest emails and return a summary
            try:
                inbox_encoded = AGENTMAIL_INBOX.replace("@", "%40")
                headers = {
                    "Authorization": f"Bearer {AGENTMAIL_API_KEY}",
                    "Content-Type": "application/json"
                }
                r = requests.get(
                    f"{AGENTMAIL_BASE}/inboxes/{inbox_encoded}/messages?limit=5&order=desc",
                    headers=headers, timeout=15
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

        if path == "/notifications":
            # Return pending email notifications
            # Only clear when ESP32 confirms with /notifications?ack=1
            notifs = list(email_notifications)
            ack = "ack=1" in (self.path or "")
            if ack:
                email_notifications.clear()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "count": len(notifs),
                "notifications": notifs
            }).encode())
            return

        # Health check
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({
            "status": "ok",
            "blender": "connected" if blender.connected else "disconnected",
            "email": AGENTMAIL_INBOX,
            "pending_notifications": len(email_notifications),
            "last_email_check": last_email_check
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

            inbox_encoded = AGENTMAIL_INBOX.replace("@", "%40")
            send_url = f"{AGENTMAIL_BASE}/inboxes/{inbox_encoded}/messages/send"
            headers = {
                "Authorization": f"Bearer {AGENTMAIL_API_KEY}",
                "Content-Type": "application/json"
            }
            reply_subject = subject if subject.startswith("Re:") else f"Re: {subject}"
            send_data = {
                "to": [reply_to],
                "subject": reply_subject,
                "body": ai_reply
            }
            sr = requests.post(send_url, json=send_data, headers=headers, timeout=15)
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
            "sent": reply_sent
        }).encode())

    def log_message(self, format, *args):
        pass


def main():
    print("=" * 50)
    print("  Pixel Bridge Server")
    print("  ESP32 → Ollama + Blender + AgentMail")
    print("=" * 50)

    print("\n[Blender] Connecting...")
    if blender.connect():
        print("[Blender] Connected!")
    else:
        print("[Blender] Not connected. Will retry on command.")

    # Start email polling thread
    email_thread = threading.Thread(target=poll_emails, daemon=True)
    email_thread.start()
    print(f"[Email] Monitoring {AGENTMAIL_INBOX}")

    port = 8888
    server = HTTPServer(("0.0.0.0", port), BridgeHandler)
    print(f"\n[Server] Listening on http://0.0.0.0:{port}")
    print("[Server] Waiting for Pixel...\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Server] Shutting down.")
        server.server_close()

if __name__ == "__main__":
    main()
