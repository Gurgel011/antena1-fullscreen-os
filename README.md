# Antena 1 Fullscreen OS

Firmware ESP32-S3 pra tela AMOLED da Waveshare, dedicado ao rádio Antena 1: toca o stream ao vivo direto no aparelho, mostra status de wifi/volume, um rosto de robô animado e uma barra com o consumo de tokens do Claude Code.

## Hardware

- Placa: Waveshare ESP32-S3 Touch AMOLED 1.75" (466x466), BSP `waveshare/esp32_s3_touch_amoled_1_75`
- Target: `esp32s3`
- UI: LVGL 9

## O que o firmware faz

- Conecta no wifi e toca `RADIO_STREAM_URL` (stream AAC da Antena 1) via `esp_audio_simple_player`, sem passar por servidor intermediário.
- Mostra status de conexão, volume e um rosto animado (`draw_robot`/`make_robot_eye` em `main.c`).
- Faz polling em `TOKEN_USAGE_URL` (servidor local, ver `tools/usage_server.py`) e anima uma barra com o consumo de tokens da sessão/semana do Claude Code.

As duas URLs (`RADIO_STREAM_URL`, `TOKEN_USAGE_URL`) estão hardcoded no topo de `main/main.c` — troque ali se o IP do servidor de uso mudar.

## Estrutura

```text
main/main.c            firmware (wifi, player, UI, polling de tokens)
components/bsp_extra/  extensão do BSP da placa
tools/                 scripts de apoio, rodam no PC, não no ESP32:
  usage_server.py        expõe /usage (consumo de tokens Claude Code) pro firmware consultar
  voice_to_codex.py       ponte de voz -> Codex
  capture_mic_wav.py      captura de áudio do microfone pra debug
```

## Build e flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <porta> flash monitor
```

## Rodando o servidor de tokens

```bash
python3 tools/usage_server.py
```

Precisa estar acessível no IP configurado em `TOKEN_USAGE_URL`. Sem ele rodando, a barra de tokens só não atualiza — o resto do firmware funciona normalmente.
