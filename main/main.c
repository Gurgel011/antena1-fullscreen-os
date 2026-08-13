#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "cJSON.h"
#include "esp_audio_simple_player.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#define TAG "antena1_touch_radio"
#define RADIO_STREAM_URL "http://antena1.newradio.it/stream.aac"
#define RADIO_MAX_RETRIES 1000
#define DEFAULT_WIFI_SSID "Antena  1  94.7  FM"
#define DEFAULT_WIFI_PASS "nat13server13"
#define WIFI_CONNECTED_BIT BIT0
#define TOKEN_USAGE_URL "http://192.168.2.160:8787/usage"
#define TOKEN_USAGE_POLL_MS 30000
#define TOKEN_USAGE_HTTP_TIMEOUT_MS 4000

#if LV_FONT_MONTSERRAT_18
#define FONT_18 (&lv_font_montserrat_18)
#else
#define FONT_18 LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_28
#define FONT_28 (&lv_font_montserrat_28)
#else
#define FONT_28 LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_48
#define FONT_48 (&lv_font_montserrat_48)
#else
#define FONT_48 LV_FONT_DEFAULT
#endif

static EventGroupHandle_t wifi_event_group;
static lv_obj_t *status_label;
static lv_obj_t *hint_label;
static lv_obj_t *volume_label;
static lv_obj_t *tokens_bar;
static lv_obj_t *tokens_glow;
static lv_obj_t *session_value_label;
static lv_obj_t *session_percent_badge;
static lv_obj_t *week_value_label;
static lv_obj_t *week_percent_badge;
static lv_obj_t *robot_eye_left;
static lv_obj_t *robot_eye_right;
static int32_t tokens_bar_value;
static esp_asp_handle_t radio_player;
static bool wifi_started;
static bool radio_playing;
static bool radio_starting;
static bool radio_should_play;
static bool radio_user_stopping;
static bool play_when_wifi_connects;
static bool radio_retrying;
static bool radio_retry_scheduled;
static int radio_retry_count;
static int radio_volume = 75;

static void radio_play(void);
static void radio_retry_later(void);
static void radio_stop(void);
static void radio_volume_delta(int delta);
static bool wifi_is_connected(void);

static void ui_set_text(lv_obj_t *label, const char *text, uint32_t color)
{
    if (bsp_display_lock(500) == ESP_OK) {
        if (label) {
            lv_label_set_text(label, text);
            lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            lv_obj_invalidate(lv_screen_active());
        }
        bsp_display_unlock();
    }
}

static void ui_set_status(const char *text, uint32_t color)
{
    ui_set_text(status_label, text, color);
}

static void ui_set_hint(const char *text)
{
    ui_set_text(hint_label, text, 0x4b5563);
}

static void ui_update_volume(void)
{
    char text[32];
    snprintf(text, sizeof(text), "Volume %d%%", radio_volume);
    ui_set_text(volume_label, text, 0x203a8f);
}

static void tokens_pulse_anim_cb(void *var, int32_t value)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, value, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, value, 0);
}

static void tokens_bar_anim_cb(void *var, int32_t value)
{
    lv_bar_set_value((lv_obj_t *)var, value, LV_ANIM_OFF);
}

static void tokens_animate_bar_to(int32_t new_value)
{
    if (!tokens_bar) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, tokens_bar);
    lv_anim_set_exec_cb(&a, tokens_bar_anim_cb);
    lv_anim_set_values(&a, tokens_bar_value, new_value);
    lv_anim_set_time(&a, 900);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    tokens_bar_value = new_value;
}

static void tokens_pulse_label(lv_obj_t *label)
{
    if (!label) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, tokens_pulse_anim_cb);
    lv_anim_set_values(&a, 256, 284);
    lv_anim_set_time(&a, 200);
    lv_anim_set_playback_time(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static uint32_t percent_to_color(double percent)
{
    if (percent >= 80) {
        return 0xf87171;
    }
    if (percent >= 50) {
        return 0xfbbf24;
    }
    return 0x4ade80;
}

static void update_data_row(lv_obj_t *value_label, lv_obj_t *badge, const char *tokens_exact, double percent)
{
    if (value_label) {
        char text[64];
        snprintf(text, sizeof(text), "%s tokens", tokens_exact);
        lv_label_set_text(value_label, text);
        tokens_pulse_label(value_label);
    }
    if (badge) {
        lv_obj_t *badge_label = lv_obj_get_child(badge, 0);
        if (badge_label) {
            lv_label_set_text_fmt(badge_label, "%.0f%%", percent);
        }
        lv_obj_set_style_bg_color(badge, lv_color_hex(percent_to_color(percent)), 0);
    }
}

static void ui_update_tokens(const char *session_tokens_exact, double session_percent, const char *week_tokens_exact,
                              double week_percent)
{
    if (bsp_display_lock(500) != ESP_OK) {
        return;
    }
    update_data_row(session_value_label, session_percent_badge, session_tokens_exact, session_percent);
    update_data_row(week_value_label, week_percent_badge, week_tokens_exact, week_percent);

    uint32_t eye_color = percent_to_color(session_percent);
    if (robot_eye_left) {
        lv_obj_set_style_bg_color(robot_eye_left, lv_color_hex(eye_color), 0);
    }
    if (robot_eye_right) {
        lv_obj_set_style_bg_color(robot_eye_right, lv_color_hex(eye_color), 0);
    }

    double clamped = session_percent < 0 ? 0 : (session_percent > 100 ? 100 : session_percent);
    tokens_animate_bar_to((int32_t)clamped);
    bsp_display_unlock();
}

typedef struct {
    char buf[512];
    int len;
} token_http_ctx_t;

static esp_err_t token_http_event_handler(esp_http_client_event_t *evt)
{
    token_http_ctx_t *ctx = (token_http_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx) {
        int copy_len = evt->data_len;
        if (ctx->len + copy_len >= (int)sizeof(ctx->buf)) {
            copy_len = (int)sizeof(ctx->buf) - ctx->len - 1;
        }
        if (copy_len > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, copy_len);
            ctx->len += copy_len;
            ctx->buf[ctx->len] = 0;
        }
    }
    return ESP_OK;
}

static void token_usage_fetch_once(void)
{
    token_http_ctx_t ctx = {0};
    esp_http_client_config_t cfg = {
        .url = TOKEN_USAGE_URL,
        .timeout_ms = TOKEN_USAGE_HTTP_TIMEOUT_MS,
        .event_handler = token_http_event_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Falha ao buscar uso de tokens: err=%d status=%d", err, status);
        return;
    }

    cJSON *root = cJSON_Parse(ctx.buf);
    if (!root) {
        ESP_LOGW(TAG, "JSON invalido no uso de tokens");
        return;
    }
    const cJSON *session_tokens = cJSON_GetObjectItemCaseSensitive(root, "session_tokens_exact");
    const cJSON *session_percent = cJSON_GetObjectItemCaseSensitive(root, "session_percent");
    const cJSON *week_tokens = cJSON_GetObjectItemCaseSensitive(root, "week_tokens_exact");
    const cJSON *week_percent = cJSON_GetObjectItemCaseSensitive(root, "week_percent");
    if (cJSON_IsString(session_tokens) && session_tokens->valuestring && cJSON_IsNumber(session_percent) &&
        cJSON_IsString(week_tokens) && week_tokens->valuestring && cJSON_IsNumber(week_percent)) {
        ui_update_tokens(session_tokens->valuestring, session_percent->valuedouble, week_tokens->valuestring,
                         week_percent->valuedouble);
        ESP_LOGI(TAG, "Uso atualizado: sessao %s tokens (%.1f%%), semana %s tokens (%.1f%%)",
                 session_tokens->valuestring, session_percent->valuedouble, week_tokens->valuestring,
                 week_percent->valuedouble);
    }
    cJSON_Delete(root);
}

static void token_usage_task(void *arg)
{
    (void)arg;
    while (true) {
        if (wifi_is_connected()) {
            token_usage_fetch_once();
        }
        vTaskDelay(pdMS_TO_TICKS(TOKEN_USAGE_POLL_MS));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ui_set_hint("Wi-Fi desconectado");
        esp_wifi_connect();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ui_set_hint("Wi-Fi conectado");
        ESP_LOGI(TAG, "Wi-Fi conectado");
        if (play_when_wifi_connects) {
            play_when_wifi_connects = false;
            radio_play();
        }
    }
}

static void wifi_start_once(void)
{
    if (wifi_started) {
        return;
    }
    wifi_started = true;
    wifi_event_group = xEventGroupCreate();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, DEFAULT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, DEFAULT_WIFI_PASS, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    ui_set_hint("Conectando Wi-Fi...");
}

static bool wifi_is_connected(void)
{
    return wifi_event_group && (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT);
}

static int radio_output_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    size_t written = 0;
    if (data_size <= 0) {
        return 0;
    }
    if (bsp_extra_i2s_write(data, data_size, &written, portMAX_DELAY) != ESP_OK) {
        return -1;
    }
    return (int)written;
}

static esp_err_t radio_prepare_output(void)
{
    ESP_RETURN_ON_ERROR(bsp_extra_codec_init(), TAG, "codec init failed");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO), TAG, "codec fs failed");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_volume_set(radio_volume, NULL), TAG, "volume failed");
    ESP_RETURN_ON_ERROR(bsp_extra_codec_mute_set(false), TAG, "unmute failed");
    return ESP_OK;
}

static int radio_event_cb(esp_asp_event_pkt_t *event, void *ctx)
{
    (void)ctx;
    if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t state = ESP_ASP_STATE_NONE;
        memcpy(&state, event->payload, event->payload_size);
        const char *state_name = esp_audio_simple_player_state_to_str(state);
        ESP_LOGI(TAG, "Radio state: %s", state_name);
        if (strstr(state_name, "RUNNING")) {
            radio_playing = true;
            radio_starting = false;
            radio_retry_count = 0;
        } else if (strstr(state_name, "ERROR")) {
            radio_playing = false;
            radio_starting = false;
            if (radio_should_play) {
                radio_retry_later();
            }
        } else if (strstr(state_name, "STOPPED") || strstr(state_name, "FINISHED")) {
            radio_playing = false;
            radio_starting = false;
            if (radio_user_stopping) {
                radio_user_stopping = false;
                ui_set_status("Radio parada", 0x111827);
            } else {
                radio_should_play = true;
                radio_retry_later();
            }
        }
    }
    return 0;
}

static esp_err_t radio_player_init_once(void)
{
    if (radio_player) {
        return ESP_OK;
    }

    esp_asp_cfg_t cfg = {
        .in = { .cb = NULL, .user_ctx = NULL },
        .out = { .cb = radio_output_cb, .user_ctx = NULL },
        .task_prio = 5,
        .task_stack = 8192,
        .task_core = 0,
        .task_stack_in_ext = true,
        .prev = NULL,
        .prev_ctx = NULL,
    };
    esp_gmf_err_t err = esp_audio_simple_player_new(&cfg, &radio_player);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Falha criando player: %d", err);
        return ESP_FAIL;
    }
    esp_audio_simple_player_set_event(radio_player, radio_event_cb, NULL);
    return ESP_OK;
}

static void radio_play(void)
{
    if (!radio_retrying) {
        radio_retry_count = 0;
    }
    radio_should_play = true;
    wifi_start_once();
    if (!wifi_is_connected()) {
        play_when_wifi_connects = true;
        ui_set_status("Conectando Wi-Fi", 0x203a8f);
        esp_wifi_connect();
        return;
    }
    if (radio_playing || radio_starting) {
        ui_set_status("Tocando Antena 1", 0x203a8f);
        return;
    }
    ui_set_status("Tocando Antena 1", 0x203a8f);
    ui_set_hint("Toque para parar");
    vTaskDelay(pdMS_TO_TICKS(300));

    if (radio_prepare_output() != ESP_OK || radio_player_init_once() != ESP_OK) {
        ui_set_status("Falha no audio", 0xd71920);
        return;
    }
    radio_starting = true;
    esp_gmf_err_t err = esp_audio_simple_player_run(radio_player, RADIO_STREAM_URL, NULL);
    if (err != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Falha ao tocar stream: %d", err);
        radio_starting = false;
        radio_retry_later();
    }
}

static void radio_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    radio_retry_scheduled = false;
    if (!radio_should_play) {
        vTaskDelete(NULL);
    }
    radio_retrying = true;
    radio_play();
    radio_retrying = false;
    vTaskDelete(NULL);
}

static void radio_retry_later(void)
{
    if (radio_retry_count >= RADIO_MAX_RETRIES) {
        ESP_LOGE(TAG, "Falha ao abrir radio apos %d tentativas", RADIO_MAX_RETRIES);
        radio_should_play = false;
        radio_retry_scheduled = false;
        radio_retry_count = 0;
        ui_set_status("Falha na radio", 0xd71920);
        ui_set_hint("Toque para tentar de novo");
        return;
    }
    if (radio_retry_scheduled) {
        return;
    }
    radio_retry_count++;
    radio_retry_scheduled = true;
    ESP_LOGW(TAG, "Tentando abrir radio novamente (%d/%d)", radio_retry_count, RADIO_MAX_RETRIES);
    if (xTaskCreate(radio_retry_task, "radio_retry", 4096, NULL, 4, NULL) != pdPASS) {
        radio_retry_scheduled = false;
    }
}

static void radio_stop(void)
{
    radio_should_play = false;
    radio_retry_scheduled = false;
    play_when_wifi_connects = false;
    if (radio_player && (radio_playing || radio_starting)) {
        radio_user_stopping = true;
        esp_audio_simple_player_stop(radio_player);
    } else {
        radio_user_stopping = false;
    }
    radio_playing = false;
    radio_starting = false;
    ui_set_status("Radio parada", 0x111827);
    ui_set_hint("Toque para tocar");
}

static void radio_volume_set(int volume)
{
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    radio_volume = volume;
    if (bsp_extra_codec_init() == ESP_OK) {
        bsp_extra_codec_volume_set(radio_volume, NULL);
    }
    ui_update_volume();
}

static void radio_volume_delta(int delta)
{
    radio_volume_set(radio_volume + delta);
    ui_set_status(delta > 0 ? "Volume aumentado" : "Volume diminuido", 0x203a8f);
}

static void command_to_lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static void handle_serial_command(char *line)
{
    command_to_lower(line);
    ESP_LOGI(TAG, "Comando serial: %s", line);

    if (strstr(line, "radio_stop") || strstr(line, "parar") || strstr(line, "pare") ||
        strstr(line, "stop") || strstr(line, "desligar") || strstr(line, "sem som") ||
        strstr(line, "silencio") || strstr(line, "silêncio")) {
        radio_stop();
    } else if (strstr(line, "vol_up") || strstr(line, "aumentar") || strstr(line, "aumenta") ||
               strstr(line, "mais volume") || strstr(line, "subir volume")) {
        radio_volume_delta(10);
    } else if (strstr(line, "vol_down") || strstr(line, "diminuir") || strstr(line, "diminui") ||
               strstr(line, "menos volume") || strstr(line, "baixar") || strstr(line, "abaixar")) {
        radio_volume_delta(-10);
    } else if (strstr(line, "radio_play") || strstr(line, "tocar") || strstr(line, "toque") ||
               strstr(line, "play") || strstr(line, "ligar radio") || strstr(line, "antena 1") ||
               strstr(line, "antena um")) {
        radio_play();
    } else if (strstr(line, "no_command") || strstr(line, "sem_comando")) {
        ui_set_status("Nao entendi", 0xd71920);
        ui_set_hint("Toque para tentar de novo");
    } else if (strstr(line, "wifi")) {
        wifi_start_once();
        esp_wifi_connect();
    } else {
        ui_set_status("Pedido nao reconhecido", 0xd71920);
        ui_set_hint("Toque para tentar de novo");
    }
}

static void serial_command_task(void *arg)
{
    (void)arg;
    char line[96];
    size_t len = 0;
    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (len > 0) {
                line[len] = 0;
                handle_serial_command(line);
                len = 0;
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        } else {
            len = 0;
        }
    }
}

static lv_obj_t *make_center_label(lv_obj_t *parent, const char *text, int y_offset, const lv_font_t *font,
                                    uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y_offset);
    return label;
}

static lv_obj_t *make_data_row(lv_obj_t *parent, int y_offset, lv_obj_t **out_value_label, lv_obj_t **out_badge)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 400, 34);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y_offset);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text(value_label, "carregando...");
    lv_obj_set_style_text_font(value_label, FONT_28, 0);
    lv_obj_set_style_text_color(value_label, lv_color_hex(0xe8ecf2), 0);

    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 58, 28);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(0x4ade80), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, "--");
    lv_obj_set_style_text_font(badge_label, FONT_18, 0);
    lv_obj_set_style_text_color(badge_label, lv_color_hex(0x0a0c10), 0);
    lv_obj_center(badge_label);

    *out_value_label = value_label;
    *out_badge = badge;
    return row;
}

static lv_obj_t *make_robot_eye(lv_obj_t *parent, int x_offset)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 11, 11);
    lv_obj_align(eye, LV_ALIGN_CENTER, x_offset, -128);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0x4ade80), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
    return eye;
}

static void draw_robot(lv_obj_t *scr)
{
    lv_obj_t *antenna_tip = lv_obj_create(scr);
    lv_obj_remove_style_all(antenna_tip);
    lv_obj_set_size(antenna_tip, 10, 10);
    lv_obj_align(antenna_tip, LV_ALIGN_CENTER, 0, -181);
    lv_obj_set_style_radius(antenna_tip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(antenna_tip, lv_color_hex(0x8fb4e3), 0);
    lv_obj_set_style_bg_opa(antenna_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(antenna_tip, 10, 0);
    lv_obj_set_style_shadow_color(antenna_tip, lv_color_hex(0x8fb4e3), 0);
    lv_obj_set_style_shadow_opa(antenna_tip, 120, 0);
    lv_obj_clear_flag(antenna_tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(antenna_tip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *antenna_stem = lv_obj_create(scr);
    lv_obj_remove_style_all(antenna_stem);
    lv_obj_set_size(antenna_stem, 3, 16);
    lv_obj_align(antenna_stem, LV_ALIGN_CENTER, 0, -168);
    lv_obj_set_style_bg_color(antenna_stem, lv_color_hex(0x5a6478), 0);
    lv_obj_set_style_bg_opa(antenna_stem, LV_OPA_COVER, 0);
    lv_obj_clear_flag(antenna_stem, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(antenna_stem, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, 66, 66);
    lv_obj_align(head, LV_ALIGN_CENTER, 0, -128);
    lv_obj_set_style_radius(head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x2a3040), 0);
    lv_obj_set_style_bg_grad_color(head, lv_color_hex(0x11151d), 0);
    lv_obj_set_style_bg_grad_dir(head, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(0x4a6fa5), 0);
    lv_obj_set_style_border_opa(head, 160, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);

    robot_eye_left = make_robot_eye(scr, -13);
    robot_eye_right = make_robot_eye(scr, 13);
}

static void draw_home_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0c10), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tokens_glow = lv_obj_create(scr);
    lv_obj_remove_style_all(tokens_glow);
    lv_obj_set_size(tokens_glow, 360, 360);
    lv_obj_align(tokens_glow, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(tokens_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tokens_glow, lv_color_hex(0x4a6fa5), 0);
    lv_obj_set_style_bg_opa(tokens_glow, 45, 0);
    lv_obj_clear_flag(tokens_glow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tokens_glow, LV_OBJ_FLAG_SCROLLABLE);

    draw_robot(scr);

    tokens_bar = lv_bar_create(scr);
    lv_obj_remove_style(tokens_bar, NULL, LV_PART_KNOB);
    lv_obj_set_size(tokens_bar, 260, 6);
    lv_obj_align(tokens_bar, LV_ALIGN_CENTER, 0, -62);
    lv_bar_set_range(tokens_bar, 0, 100);
    lv_obj_clear_flag(tokens_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(tokens_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tokens_bar, lv_color_hex(0x1c2028), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tokens_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(tokens_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(tokens_bar, lv_color_hex(0x4a6fa5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(tokens_bar, lv_color_hex(0x8fb4e3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(tokens_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_bar_set_value(tokens_bar, 0, LV_ANIM_OFF);
    tokens_bar_value = 0;

    make_center_label(scr, "SESSAO", -30, FONT_18, 0x6f7a91);
    make_data_row(scr, 0, &session_value_label, &session_percent_badge);
    make_center_label(scr, "SEMANA", 38, FONT_18, 0x6f7a91);
    make_data_row(scr, 68, &week_value_label, &week_percent_badge);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando Antena 1 por toque");

    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "Falha ao iniciar display");
        return;
    }
    bsp_display_backlight_on();

    lv_indev_t *indev = bsp_display_get_input_dev();
    ESP_LOGI(TAG, "Display iniciado. Input device=%p tipo=%d",
             indev, indev ? (int)lv_indev_get_type(indev) : -1);

    if (bsp_display_lock(1000) == ESP_OK) {
        draw_home_screen();
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

    xTaskCreate(serial_command_task, "serial_cmd", 4096, NULL, 4, NULL);
    xTaskCreate(token_usage_task, "token_usage", 4096, NULL, 3, NULL);
    wifi_start_once();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
