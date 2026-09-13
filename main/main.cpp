#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/param.h>

static const char *TAG = "ExplorerFW";

#define BOOT_BTN_PIN GPIO_NUM_0

// Variáveis da Interface Gráfica
static lv_obj_t *lbl_status = NULL;

// Referência ao arquivo index.html embutido pelo CMake (EMBED_FILES)
extern "C" const uint8_t index_html_start[] asm("_binary_index_html_start");
extern "C" const uint8_t index_html_end[]   asm("_binary_index_html_end");

// =========================================================
// FUNÇÕES DE SAÍDA (VOLTAR PARA O FACTORY FIRMWARE)
// =========================================================
static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    
    // Feedback visual imediato
    if (lvgl_port_lock(0)) {
        bsp_display_brightness_set(0); 
        lvgl_port_unlock();
    }

    const esp_partition_t *factory_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, 
        ESP_PARTITION_SUBTYPE_APP_FACTORY, 
        NULL
    );
    
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Factory partition nao encontrada!");
        esp_restart();
    }
}

// =========================================================
// HANDLERS DO SERVIDOR HTTP (API)
// =========================================================

// 1. Entrega a página HTML embutida
static esp_err_t index_get_handler(httpd_req_t *req) {
    const size_t html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

// 2. Lista os arquivos do diretório (Retorna JSON)
static esp_err_t api_list_handler(httpd_req_t *req) {
    char dir_path[256] = "/sdcard";
    
    // Pega o parâmetro '?dir=' da URL (se existir)
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "dir", param, sizeof(param)) == ESP_OK) {
                // Decodifica a URL (Ex: %20 para espaço)
                // Para simplificar, confiamos no caminho cru que o JS manda limpo
                strncpy(dir_path, param, sizeof(dir_path));
            }
        }
        free(buf);
    }

    cJSON *root = cJSON_CreateArray();
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", ent->d_name);
            cJSON_AddStringToObject(item, "type", (ent->d_type == DT_DIR) ? "dir" : "file");
            
            if (ent->d_type != DT_DIR) {
                char full_path[512];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);
                struct stat st;
                if (stat(full_path, &st) == 0) {
                    cJSON_AddNumberToObject(item, "size", st.st_size);
                } else {
                    cJSON_AddNumberToObject(item, "size", 0);
                }
            }
            cJSON_AddItemToArray(root, item);
        }
        closedir(dir);
    }

    const char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    
    free((void *)json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// 3. Recebe o upload do arquivo em pedaços (Chunks de alta velocidade)
static esp_err_t api_upload_handler(httpd_req_t *req) {
    char file_path[256] = "/sdcard/upload.bin";
    
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "path", file_path, sizeof(file_path));
        }
        free(buf);
    }

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erro ao criar arquivo no SD");
        return ESP_FAIL;
    }

    // Buffer otimizado de 16KB para máxima taxa de escrita no SD
    char *recv_buf = (char *)malloc(16384); 
    if (!recv_buf) {
        fclose(f);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, recv_buf, MIN(remaining, 16384));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue; // Tenta de novo se houver timeout na rede
            fclose(f);
            free(recv_buf);
            return ESP_FAIL;
        }
        fwrite(recv_buf, 1, received, f);
        remaining -= received;
    }

    fclose(f);
    free(recv_buf);
    
    httpd_resp_sendstr(req, "Upload Concluido");
    return ESP_OK;
}

// 4. Excluir Arquivo
static esp_err_t api_delete_handler(httpd_req_t *req) {
    char file_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "path", file_path, sizeof(file_path));
        }
        free(buf);
    }
    
    remove(file_path);
    httpd_resp_sendstr(req, "Deletado");
    return ESP_OK;
}

// 5. Criar Diretório
static esp_err_t api_mkdir_handler(httpd_req_t *req) {
    char dir_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "path", dir_path, sizeof(dir_path));
        }
        free(buf);
    }
    
    mkdir(dir_path, 0777);
    httpd_resp_sendstr(req, "Diretorio Criado");
    return ESP_OK;
}

// =========================================================
// INICIALIZAÇÃO DO SERVIDOR WEB E WI-FI
// =========================================================
static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.core_id = 0; // Fixa na CPU0 para deixar a CPU1 livre (ou vice-versa)
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        
        httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_index);

        httpd_uri_t uri_list = { .uri = "/api/list", .method = HTTP_GET, .handler = api_list_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_list);

        httpd_uri_t uri_upload = { .uri = "/api/upload", .method = HTTP_POST, .handler = api_upload_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_upload);

        httpd_uri_t uri_delete = { .uri = "/api/delete", .method = HTTP_POST, .handler = api_delete_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_delete);
        
        httpd_uri_t uri_mkdir = { .uri = "/api/mkdir", .method = HTTP_POST, .handler = api_mkdir_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_mkdir);
        
        ESP_LOGI(TAG, "Servidor Web Iniciado!");
    }
}

static void wifi_init_softap(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.ap.ssid, "WatchOS-Explorer");
    strcpy((char *)wifi_config.ap.password, "12345678"); // Senha simples
    wifi_config.ap.ssid_len = strlen("WatchOS-Explorer");
    wifi_config.ap.channel = 6;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Wi-Fi AP Iniciado. SSID: WatchOS-Explorer | Senha: 12345678");
}

// =========================================================
// INTERFACE GRÁFICA DO RELÓGIO (LVGL)
// =========================================================
static void build_ui() {
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);

    // Título
    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_WIFI " Servidor Web Ativo");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    // Box de Informações da Rede
    lv_obj_t * info_box = lv_obj_create(scr);
    lv_obj_set_size(info_box, 300, 160);
    lv_obj_align(info_box, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(info_box, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(info_box, lv_color_hex(0x007BFF), 0);
    lv_obj_set_style_border_width(info_box, 2, 0);

    lbl_status = lv_label_create(info_box);
    lv_label_set_text(lbl_status, "Conecte-se a rede Wi-Fi:\n\nSSID: WatchOS-Explorer\nSenha: 12345678\n\nAcesse no Navegador:\nhttp://192.168.4.1");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00FF00), 0); // Verde terminal
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl_status);

    // Botão de Voltar ao Factory Firmware
    lv_obj_t * btn_exit = lv_btn_create(scr);
    lv_obj_set_size(btn_exit, 240, 60);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0xCC0000), 0); // Vermelho de Atenção
    lv_obj_set_style_radius(btn_exit, 30, 0);
    
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER " Encerrar e Voltar");
    lv_obj_set_style_text_font(lbl_exit, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_exit);
    
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){
        return_to_factory();
    }, LV_EVENT_CLICKED, NULL);
}

// =========================================================
// MAIN ENTRY POINT
// =========================================================
extern "C" void app_main(void) {
    // 1. Configura botão físico BOOT como plano de emergência
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // 2. Inicializações Base (NVS, Display, Cartão SD)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();
    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(100)); 
    bsp_display_brightness_set(80);

    bsp_sdcard_mount();
    vTaskDelay(pdMS_TO_TICKS(500)); // Aguarda a montagem mecânica
    
    // Agora que os pinos físicos estão ligados, subimos a camada lógica
    SdUsbManager::get_instance().init_local_storage();

    // 3. Inicia a Máquina de Rede
    wifi_init_softap();
    start_webserver();

    ESP_LOGI(TAG, "Explorer Firmware 100%% Ativo. Aguardando conexões...");
    
    // 4. Task Monitora o botão BOOT (Failsafe Hardware)
    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) {
            ESP_LOGW(TAG, "Botão BOOT acionado manualmente.");
            return_to_factory();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}