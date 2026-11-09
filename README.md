# Antena 1 · Fullscreen OS

`ESP32-S3` · `ESP-IDF` · `C` · `LVGL 9` · `AMOLED`

[Funcionalidades](#o-que-o-firmware-faz) · [Configuração](#configuração) · [Build](#build-e-flash) · [← Todos os projetos](https://github.com/Gurgel011#projetos)

Firmware ESP32-S3 pra tela AMOLED da Waveshare, dedicado ao rádio Antena 1: toca o stream ao vivo direto no aparelho, mostra status de wifi/volume, um rosto de robô animado e uma barra com o consumo de tokens do Claude Code.

## Hardware

- Placa: Waveshare ESP32-S3 Touch AMOLED 1.75" (466x466), BSP `waveshare/esp32_s3_touch_amoled_1_75`
- Target: `esp32s3`
- UI: LVGL 9

## O que o firmware faz

- Conecta no wifi e toca `RADIO_STREAM_URL` (stream AAC da Antena 1) via `esp_audio_simple_player`, sem passar por servidor intermediário.
- Mostra status de conexão, volume e um rosto animado (`draw_robot`/`make_robot_eye` em `main.c`).
- Faz polling em `TOKEN_USAGE_URL` (servidor local, ver `tools/usage_server.py`) e anima uma barra com o consumo de tokens da sessão/semana do Claude Code.

`RADIO_STREAM_URL` é pública e fica hardcoded no topo de `main/main.c`. Wi-Fi e
o endereço do servidor de tokens (`TOKEN_USAGE_URL`) ficam em
`main/local_secrets.h`, que **não é versionado** — ver [Configuração](#configuração).

## Estrutura

```text
main/main.c                  firmware (wifi, player, UI, polling de tokens)
main/local_secrets.h          config local (Wi-Fi, IP do servidor) — não versionado
main/local_secrets.h.example  modelo público, sem valores reais
components/bsp_extra/        extensão do BSP da placa
tools/                        scripts de apoio, rodam no PC, não no ESP32:
  usage_server.py               expõe /usage (consumo de tokens Claude Code) pro firmware consultar
  voice_to_codex.py             ponte de voz -> Codex (usa faster-whisper local)
  capture_mic_wav.py            captura de áudio do microfone pra debug
  antena1-voz / codex-voz        atalhos de shell pro voice_to_codex.py
```

## Configuração

```bash
cp main/local_secrets.h.example main/local_secrets.h
# edite main/local_secrets.h com o SSID/senha do Wi-Fi e o IP do servidor de tokens
```

Sem esse arquivo, o firmware ainda compila (usa valores de espaço reservado),
mas não conecta em nenhuma rede.

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

Precisa estar acessível no IP/porta configurados em `main/local_secrets.h`
(`TOKEN_USAGE_HOST`/`TOKEN_USAGE_PORT`). Sem ele rodando, a barra de tokens só
não atualiza — o resto do firmware funciona normalmente.

## Ponte de voz para o Codex (`tools/voice_to_codex.py`)

Grava um pedido falado pelo microfone do computador (ou recebido do aparelho
via serial), transcreve com faster-whisper local, e encaminha o texto para a
CLI do Codex atuar no próprio repositório.

```bash
export ANTENA1_SERIAL_PORT=/dev/ttyACM0   # ajuste para a sua porta
tools/antena1-voz
```

Arquivos temporários (áudio capturado, log da última execução) ficam em
`tmp/`, não versionado.
