#!/usr/bin/env python3
"""Tiny OpenAI-compatible mock used by test_llm_backend. Streams SSE like llama-server does."""
import json, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY = "key123"

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def _json(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

    def do_GET(self):
        self._json(200, {"ok": True}) if self.path == "/health" else self._json(404, {"error": "nope"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = json.loads(self.rfile.read(n) or b"{}")
        if self.headers.get("Authorization") != "Bearer " + KEY:
            return self._json(401, {"error": {"message": "bad key"}})
        if self.path != "/v1/chat/completions":
            return self._json(404, {"error": {"message": "unknown path"}})
        last = body["messages"][-1]
        text = last["content"] if isinstance(last["content"], str) else " ".join(p.get("text", "") for p in last["content"] if p["type"] == "text")
        self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
        def send(obj):
            data = ("data: " + (obj if isinstance(obj, str) else json.dumps(obj)) + "\n\n").encode()
            self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n"); self.wfile.flush()
        def delta(**d): return {"choices": [{"index": 0, "delta": d, "finish_reason": None}]}
        def finish(reason): return {"choices": [{"index": 0, "delta": {}, "finish_reason": reason}]}
        if text.startswith("error400"):
            # emulate an in-stream error object
            send({"error": {"message": "context too long", "code": 400}})
        elif text.startswith("tool"):
            send(delta(role="assistant", tool_calls=[{"index": 0, "id": "call_9", "type": "function", "function": {"name": "echo", "arguments": ""}}]))
            for part in ['{"x"', ':"hel', 'lo"}']:
                send(delta(tool_calls=[{"index": 0, "function": {"arguments": part}}]))
            send(delta(tool_calls=[{"index": 1, "id": "call_10", "type": "function", "function": {"name": "second", "arguments": "{}"}}]))
            send(finish("tool_calls"))
        elif text.startswith("reason"):
            send(delta(reasoning_content="think ")); send(delta(reasoning_content="more")); send(delta(content="answer")); send(finish("stop"))
        elif text.startswith("image"):
            ok = isinstance(last["content"], list) and any(p["type"] == "image_url" and p["image_url"]["url"].startswith("data:image/png") for p in last["content"])
            send(delta(content="saw image" if ok else "NO IMAGE")); send(finish("stop"))
        elif text.startswith("slow"):
            for i in range(200):
                try: send(delta(content="tok%d " % i))
                except Exception: return
                time.sleep(0.05)
            send(finish("stop"))
        elif text.startswith("meta"):
            # prove sampling params and tools reach the server
            info = {"tools": [t["function"]["name"] for t in body.get("tools", [])], "temp": body.get("temperature"), "model": body.get("model"),
                    "roles": [m["role"] for m in body["messages"]], "tool_call_id": [m.get("tool_call_id") for m in body["messages"] if m["role"] == "tool"],
                    "assistant_calls": [len(m.get("tool_calls", [])) for m in body["messages"] if m["role"] == "assistant"]}
            send(delta(content=json.dumps(info))); send(finish("stop"))
        else:
            for w in ["Hel", "lo ", "wor", "ld"]: send(delta(content=w))
            send(finish("stop"))
            send({"choices": [], "usage": {"prompt_tokens": 11, "completion_tokens": 4}, "timings": {"predicted_per_second": 42.5}})
        send("[DONE]"); self.wfile.write(b"0\r\n\r\n"); self.wfile.flush()

if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
