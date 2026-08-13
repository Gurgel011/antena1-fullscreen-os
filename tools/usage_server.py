#!/usr/bin/env python3
"""Serve o consumo de tokens do Claude Code (sessao de 5h e semana) para o painel do ESP32.

Endpoint: GET /usage -> {
    "session_tokens": int, "session_tokens_exact": str, "session_percent": float,
    "week_tokens": int, "week_tokens_exact": str, "week_percent": float,
    "updated_at": str
}

Os limites abaixo foram CALIBRADOS a partir dos percentuais reais mostrados em
claude.ai (tela de uso da conta). Ultima calibracao em 2026-07-13 15:22, com
sessao=10.212.629 tokens=30% e semana(todos os modelos)=53.463.308 tokens=4%.
Ainda e uma estimativa (o calculo real da Anthropic e proprietario e ponderado
por modelo, varia conforme o mix de modelos/cache usado), mas calibrada com um
ponto de dado real em vez de um chute. Recalibrar de tempos em tempos: pegar
os tokens brutos deste endpoint e o % real em claude.ai no mesmo instante,
depois limite_novo = tokens_brutos / (percentual / 100).
"""
import json
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "0.0.0.0"
PORT = 8787
CACHE_TTL_SECONDS = 20
SESSION_TOKEN_LIMIT_ESTIMATE = 34_042_097
WEEKLY_TOKEN_LIMIT_ESTIMATE = 1_336_582_700

_cache_lock = threading.Lock()
_cache = {"data": None, "fetched_at": 0.0}


def format_exact(n):
    return f"{n:,}".replace(",", ".")


def fetch_usage():
    session_result = subprocess.run(
        ["npx", "ccusage@latest", "claude", "blocks", "--active", "--json"],
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    session_payload = json.loads(session_result.stdout)
    active_blocks = session_payload.get("blocks") or []
    session_tokens = active_blocks[0].get("totalTokens", 0) if active_blocks else 0

    week_result = subprocess.run(
        ["npx", "ccusage@latest", "claude", "weekly", "--json"],
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    week_payload = json.loads(week_result.stdout)
    weekly = week_payload.get("weekly") or []
    week_tokens = weekly[-1].get("totalTokens", 0) if weekly else 0

    session_percent = min(100.0, (session_tokens / SESSION_TOKEN_LIMIT_ESTIMATE) * 100)
    week_percent = min(100.0, (week_tokens / WEEKLY_TOKEN_LIMIT_ESTIMATE) * 100)

    return {
        "session_tokens": session_tokens,
        "session_tokens_exact": format_exact(session_tokens),
        "session_percent": round(session_percent, 1),
        "week_tokens": week_tokens,
        "week_tokens_exact": format_exact(week_tokens),
        "week_percent": round(week_percent, 1),
        "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }


def get_usage_cached():
    with _cache_lock:
        age = time.time() - _cache["fetched_at"]
        if _cache["data"] is not None and age < CACHE_TTL_SECONDS:
            return _cache["data"]
    data = fetch_usage()
    with _cache_lock:
        _cache["data"] = data
        _cache["fetched_at"] = time.time()
    return data


class UsageHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path != "/usage":
            self.send_response(404)
            self.end_headers()
            return
        try:
            data = get_usage_cached()
            body = json.dumps(data).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            body = json.dumps({"error": str(exc)}).encode("utf-8")
            self.send_response(500)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


def main():
    server = ThreadingHTTPServer((HOST, PORT), UsageHandler)
    print(f"antena1 usage_server ouvindo em {HOST}:{PORT}")
    server.serve_forever()


if __name__ == "__main__":
    main()
