"""A deliberately NON-deterministic OpenAI-compatible chat server for tests.

Every response samples fresh randomness (words, tool arguments, chunking,
latency), exactly like a real model at temperature > 0. If Khonsu's replay were
not faithful, the integration tests would see different agent output.

Supports: POST /v1/chat/completions, JSON or SSE streaming, tool calls, usage.
Usage: python mock_llm.py PORT   (prints "ready" when listening)
"""

import json
import os
import random
import sys
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WORDS = ["moon", "river", "falcon", "ember", "glyph", "reed", "dune", "lotus", "obelisk", "scarab", "nile", "sistrum"]
DELAY = float(os.environ.get("MOCK_DELAY", "0.002"))
rng = random.SystemRandom()  # OS entropy: never reproducible


def completion_text(messages):
    last = messages[-1]
    if last.get("role") == "tool":
        return f"Based on the tool result ({last['content']}), the answer is {rng.choice(WORDS)}-{rng.randint(0, 9999)}."
    return " ".join(rng.choice(WORDS) for _ in range(rng.randint(4, 9))) + f" #{rng.randint(0, 99999)}"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # quiet
        pass

    def do_POST(self):
        length = int(self.headers.get("content-length", "0"))
        req = json.loads(self.rfile.read(length) or b"{}")
        if self.path != "/v1/chat/completions":
            self.send_error(404)
            return
        messages = req.get("messages", [])
        wants_tool = bool(req.get("tools")) and not any(m.get("role") == "tool" for m in messages)
        cid = "chatcmpl-" + uuid.uuid4().hex[:12]
        created = int(time.time())
        usage = {"prompt_tokens": sum(len(str(m.get("content", ""))) // 4 for m in messages) + 1,
                 "completion_tokens": rng.randint(5, 40)}
        usage["total_tokens"] = usage["prompt_tokens"] + usage["completion_tokens"]
        time.sleep(DELAY * rng.randint(1, 5))

        if wants_tool:
            city = rng.choice(["Thebes", "Memphis", "Luxor", "Aswan", "Giza"])
            message = {"role": "assistant", "content": None, "tool_calls": [{
                "id": "call_" + uuid.uuid4().hex[:8], "type": "function",
                "function": {"name": "get_weather", "arguments": json.dumps({"city": city})}}]}
            finish = "tool_calls"
        else:
            message = {"role": "assistant", "content": completion_text(messages)}
            finish = "stop"

        if not req.get("stream"):
            body = json.dumps({"id": cid, "object": "chat.completion", "created": created, "model": req.get("model"),
                               "choices": [{"index": 0, "message": message, "finish_reason": finish}],
                               "usage": usage}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Server-sent events, chunked, with random chunk sizes and pauses.
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        def send(obj):
            data = ("data: " + (obj if isinstance(obj, str) else json.dumps(obj)) + "\n\n").encode()
            self.wfile.write(f"{len(data):x}\r\n".encode() + data + b"\r\n")
            self.wfile.flush()
            time.sleep(DELAY)

        base = {"id": cid, "object": "chat.completion.chunk", "created": created, "model": req.get("model")}
        text = message.get("content") or ""
        pos = 0
        while pos < len(text):
            n = rng.randint(1, 6)
            send({**base, "choices": [{"index": 0, "delta": {"content": text[pos:pos + n]}, "finish_reason": None}]})
            pos += n
        send({**base, "choices": [{"index": 0, "delta": {}, "finish_reason": finish}], "usage": usage})
        send("[DONE]")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # The crash tests kill the proxy mid-stream; a vanished client is expected.
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            super().handle_error(request, client_address)


if __name__ == "__main__":
    server = Server(("127.0.0.1", int(sys.argv[1])), Handler)
    print("ready", flush=True)
    server.serve_forever()
