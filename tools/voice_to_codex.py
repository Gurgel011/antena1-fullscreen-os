#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

from faster_whisper import WhisperModel


# Porta serial: ajuste para o seu aparelho, ou passe --port.
# Descubra a sua com: ls /dev/serial/by-id/
PORT = os.environ.get("ANTENA1_SERIAL_PORT", "/dev/ttyACM0")
ROOT = Path(__file__).resolve().parent.parent
TMP_DIR = ROOT / "tmp"
RAW_WAV = TMP_DIR / "esp32_voice_raw.wav"
LOUD_WAV = TMP_DIR / "esp32_voice_loud.wav"
CODEX_LOG = TMP_DIR / "codex_voz_ultimo.log"
MODEL_ROOT = Path.home() / ".cache/huggingface/hub/models--Systran--faster-whisper-small/snapshots"


def status(text):
    print(f"\n==> {text}", flush=True)


def run(cmd, *, show_command=True, **kwargs):
    if show_command:
        print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, check=True, **kwargs)


def reset_device(port):
    status("Resetando o aparelho para preparar a captura")
    run([
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "--port",
        port,
        "--after",
        "hard_reset",
        "chip_id",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def capture(port, out):
    script = ROOT / "tools" / "capture_mic_wav.py"
    status("Aguardando toque na tela e recebendo audio do aparelho")
    run([str(script), port, str(out)])


def amplify(raw, loud):
    status("Tratando o audio capturado para melhorar a transcricao")
    run([
        "ffmpeg",
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(raw),
        "-af",
        "pan=mono|c0=c0,volume=20",
        str(loud),
    ])


def transcribe(wav):
    status("Transcrevendo audio com faster-whisper")
    snapshots = sorted(MODEL_ROOT.iterdir())
    if not snapshots:
        raise RuntimeError(f"Modelo faster-whisper nao encontrado em {MODEL_ROOT}")

    model = WhisperModel(str(snapshots[0]), device="cpu", compute_type="int8")
    segments, _info = model.transcribe(
        str(wav),
        language="pt",
        vad_filter=False,
        beam_size=5,
        temperature=0,
    )
    return " ".join(segment.text.strip() for segment in segments).strip()


def normalize_text(text):
    return " ".join(text.lower().strip().split())


def radio_command_from_text(text):
    t = normalize_text(text)
    if not t:
        return None

    if any(word in t for word in (
        "aumentar volume",
        "aumenta volume",
        "aumente volume",
        "volume mais",
        "mais volume",
        "subir volume",
        "sobe volume",
        "aumentar",
        "aumenta",
    )):
        return "VOL_UP"
    if any(word in t for word in (
        "diminuir volume",
        "diminui volume",
        "diminua volume",
        "volume menos",
        "menos volume",
        "baixar volume",
        "abaixar volume",
        "baixa volume",
        "abaixa volume",
        "diminuir",
        "diminui",
        "abaixa",
        "baixar",
    )):
        return "VOL_DOWN"
    if any(word in t for word in (
        "parar radio",
        "para radio",
        "parar rádio",
        "para rádio",
        "pare radio",
        "pare rádio",
        "pausar radio",
        "pausar rádio",
        "desligar radio",
        "desligar rádio",
        "desliga radio",
        "desliga rádio",
        "desligar som",
        "desliga som",
        "parar audio",
        "parar áudio",
        "para audio",
        "para áudio",
        "sem som",
        "silencio",
        "silêncio",
        "sair",
    )):
        return "RADIO_STOP"
    if any(word in t for word in (
        "tocar radio",
        "toca radio",
        "toque radio",
        "tocar rádio",
        "toca rádio",
        "toque rádio",
        "ligar radio",
        "liga radio",
        "ligar rádio",
        "liga rádio",
        "play radio",
        "play rádio",
        "tocar antena",
        "toca antena",
        "antena 1",
        "antena um",
    )):
        return "RADIO_PLAY"
    if "wifi" in t or "wi-fi" in t:
        return "WIFI"
    return None


def send_device_command(port, command):
    status(f"Enviando comando direto ao aparelho: {command}")
    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
    try:
        os.write(fd, (command + "\n").encode("ascii"))
        time.sleep(0.2)
    finally:
        os.close(fd)


def send_to_codex(text):
    status("Enviando pedido transcrito ao Codex")
    print("\nCODEx recebeu este texto:", flush=True)
    print(f"\"{text}\"\n", flush=True)

    prompt = (
        "Pedido recebido por audio transcrito.\n\n"
        "Contexto fixo desta maquina:\n"
        f"- O projeto principal do aparelho ESP32-S3 Touch AMOLED esta em {ROOT}.\n"
        "- Quando o pedido mencionar tela, aparelho, logo, Antena 1, radio, microfone, voz, "
        "firmware ou ESP32, assuma esse projeto como alvo.\n"
        "- O logo atual esta em main/antena1_logo.c e main/antena1_logo.h.\n"
        "- A UI principal esta em main/main.c.\n"
        f"- Para gravar no aparelho, use a porta {PORT}.\n"
        "- Se o pedido for razoavelmente claro, tome decisoes praticas e execute; "
        "nao peca caminho de arquivo que ja esta definido acima.\n"
        "- So deixe de executar se a transcricao estiver vazia, incoerente, perigosa, "
        "ou se faltar uma informacao que nao possa ser inferida.\n\n"
        "Formato obrigatorio da sua resposta:\n"
        "1. Antes de executar qualquer comando ou editar arquivo, escreva uma linha começando com "
        "`ENTENDI:` resumindo exatamente o que voce entendeu do audio.\n"
        "2. Em seguida escreva uma linha começando com `EXECUTANDO:` dizendo a acao concreta que vai tentar.\n"
        "3. Depois execute a alteracao, compile e grave no aparelho quando a mudanca for de firmware/UI.\n"
        "4. No final escreva `RESULTADO:` dizendo se executou, se gravou, ou por que nao executou.\n\n"
        "Pedido transcrito:\n"
        f"{text}"
    )

    status("Codex esta executando. Aguarde o RESULTADO antes de falar outro pedido")
    cmd = [
        "codex",
        "exec",
        "--cd",
        str(ROOT),
        "--skip-git-repo-check",
        "--dangerously-bypass-approvals-and-sandbox",
        prompt,
    ]
    proc = subprocess.run(
        cmd,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    CODEX_LOG.write_text(proc.stdout, encoding="utf-8", errors="replace")

    print("\n========== STATUS DO CODEX ==========", flush=True)
    important = []
    for line in proc.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith(("ENTENDI:", "EXECUTANDO:", "RESULTADO:")):
            important.append(stripped)

    if important:
        for line in important:
            print(line, flush=True)
    else:
        tail = proc.stdout.splitlines()[-25:]
        print("Nao encontrei ENTENDI/EXECUTANDO/RESULTADO na saida. Ultimas linhas:", flush=True)
        for line in tail:
            print(line, flush=True)
    print("=====================================", flush=True)
    print(f"Log completo: {CODEX_LOG}", flush=True)

    if proc.returncode != 0:
        raise subprocess.CalledProcessError(proc.returncode, cmd)

    status("Codex terminou este pedido")


def main():
    parser = argparse.ArgumentParser(description="Captura audio do ESP32, transcreve e envia comandos ao aparelho.")
    parser.add_argument("--port", default=PORT)
    parser.add_argument("--raw", default=str(RAW_WAV))
    parser.add_argument("--loud", default=str(LOUD_WAV))
    parser.add_argument("--no-reset", action="store_true")
    parser.add_argument("--codex", action="store_true", help="Ativa comandos diretos para o aparelho e fallback por Codex.")
    parser.add_argument("--loop", action="store_true", help="Fica rodando e aguarda varios toques/falas.")
    args = parser.parse_args()

    raw = Path(args.raw)
    loud = Path(args.loud)
    TMP_DIR.mkdir(parents=True, exist_ok=True)

    if not args.no_reset:
        reset_device(args.port)

    while True:
        print("\n============================================================", flush=True)
        print("Toque na tela do aparelho e fale por ate 8 segundos quando aparecer Ouvindo.", flush=True)
        print("Quando terminar, vou mostrar o que entendi e enviar o comando ao aparelho.", flush=True)
        capture(args.port, raw)
        amplify(raw, loud)
        text = transcribe(loud)

        print("\n========== O QUE FOI ENTENDIDO ==========", flush=True)
        print(text if text else "[vazio]", flush=True)
        print("=========================================", flush=True)
        print(f"\nAudio bruto: {raw}")
        print(f"Audio amplificado: {loud}")

        if args.codex:
            device_command = radio_command_from_text(text) if text else None
            if device_command:
                print(f"\nCOMANDO DIRETO PARA O APARELHO: {device_command}", flush=True)
                send_device_command(args.port, device_command)
            else:
                print("\nComando nao reconhecido; vou liberar o aparelho para novo toque.", flush=True)
                send_device_command(args.port, "NO_COMMAND")

        if not args.loop:
            break

        print("\nPronto para o proximo comando por voz. Pressione Ctrl+C para parar.", flush=True)


if __name__ == "__main__":
    main()
