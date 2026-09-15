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
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>  // ADICIONADO: Necessário em C++ para malloc() e free()
#include <stdio.h>   // ADICIONADO: Necessário em C++ para fopen() e snprintf()

static const char *TAG = "ExplorerFW";
#define BOOT_BTN_PIN GPIO_NUM_0

static lv_obj_t *lbl_status = NULL;

extern "C" const uint8_t index_html_start[] asm("_binary_index_html_start");
extern "C" const uint8_t index_html_end[]   asm("_binary_index_html_end");

static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    if (lvgl_port_lock(0)) {
        bsp_display_brightness_set(0); 
        lvgl_port_unlock();
    }
    const esp_partition_t *factory_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL
    );
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        esp_restart();
    }
}

// ---------------------------------------------------------
// FUNÇÃO EXCLUSÃO RECURSIVA AGRESSIVA
// ---------------------------------------------------------
static int delete_recursively(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) return -1;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            
            char *full_path = (char*)malloc(768);
            if (full_path) {
                snprintf(full_path, 768, "%s/%s", path, ent->d_name);
                
                struct stat child_st;
                if (stat(full_path, &child_st) == 0 && S_ISDIR(child_st.st_mode)) {
                    delete_recursively(full_path);
                } else {
                    remove(full_path); 
                }
                free(full_path);
            }
        }
        closedir(dir);
        // Desbloqueio do FATFS: unlink serve tanto para arquivo quanto para forçar diretório vazio
        int res = rmdir(path);
        if (res != 0) res = unlink(path);
        return res;
    } else {
        return remove(path); 
    }
}

static char *urldecode(const char *str) {
    char *new_string = strdup(str);
    char *ptr = new_string;
    while (ptr[0] && ptr[1] && ptr[2]) {
        if (ptr[0] == '%' && isxdigit((unsigned char)ptr[1]) && isxdigit((unsigned char)ptr[2])) {
            char hex[] = {ptr[1], ptr[2], 0};
            *ptr = strtol(hex, NULL, 16);
            memmove(ptr + 1, ptr + 3, strlen(ptr + 3) + 1);
        }
        ptr++;
    }
    return new_string;
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    const size_t html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

static esp_err_t api_list_handler(httpd_req_t *req) {
    char dir_path[256] = "/sdcard";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "dir", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(dir_path, decoded, sizeof(dir_path));
                free(decoded);
            }
        }
        free(buf);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_AddArrayToObject(root, "files");

    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", ent->d_name);
            cJSON_AddBoolToObject(item, "is_dir", (ent->d_type == DT_DIR));
            if (ent->d_type != DT_DIR) {
                char *full_path = (char*)malloc(768);
                if (full_path) {
                    snprintf(full_path, 768, "%s/%s", dir_path, ent->d_name);
                    struct stat st;
                    if (stat(full_path, &st) == 0) cJSON_AddNumberToObject(item, "size", st.st_size);
                    else cJSON_AddNumberToObject(item, "size", 0);
                    free(full_path);
                }
            } else cJSON_AddNumberToObject(item, "size", 0);
            cJSON_AddItemToArray(array, item);
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

static esp_err_t api_upload_handler(httpd_req_t *req) {
    char file_path[256] = "/sdcard/upload.bin";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(file_path, decoded, sizeof(file_path));
                free(decoded);
            }
        }
        free(buf);
    }

    FILE *f = fopen(file_path, "wb");
    if (!f) return ESP_FAIL;

    // Buffer alinhado com o barramento SPI (4KB)
    char *recv_buf = (char *)malloc(4096); 
    if (!recv_buf) { fclose(f); return ESP_FAIL; }

    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, recv_buf, MIN(remaining, 4096));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        fwrite(recv_buf, 1, received, f);
        remaining -= received;
        
        // PREVENÇÃO DE BROWNOUT: Dá 5ms para o Cartão SD gravar a página 
        // física sem causar pico de corrente.
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    fflush(f);
    fsync(fileno(f)); 
    fclose(f);
    
    free(recv_buf);
    httpd_resp_sendstr(req, "Upload Concluido");
    return ESP_OK;
}

static esp_err_t api_delete_handler(httpd_req_t *req) {
    char file_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* decoded = urldecode(param);
                strncpy(file_path, decoded, sizeof(file_path));
                free(decoded);
            }
        }
        free(buf);
    }
    
    delete_recursively(file_path);
    httpd_resp_sendstr(req, "Deletado");
    return ESP_OK;
}

static esp_err_t api_mkdir_handler(httpd_req_t *req) {
    char dir_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                 char* decoded = urldecode(param);
                 strncpy(dir_path, decoded, sizeof(dir_path));
                 free(decoded);
            }
        }
        free(buf);
    }
    
    // Cria a pasta de forma limpa e natural
    mkdir(dir_path, 0777);
    
    // ATRASO DE SEGURANÇA: Dá meio segundo para o Cartão SD consolidar a 
    // Tabela FAT fisicamente antes que qualquer upload seja iniciado.
    vTaskDelay(pdMS_TO_TICKS(500)); 
    
    httpd_resp_sendstr(req, "Diretorio Criado");
    return ESP_OK;
}

static esp_err_t api_rename_handler(httpd_req_t *req) {
    char old_path[256] = "", new_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param_o[256], param_n[256];
            if (httpd_query_key_value(buf, "old", param_o, sizeof(param_o)) == ESP_OK) {
                char* dec = urldecode(param_o); strncpy(old_path, dec, sizeof(old_path)); free(dec);
            }
            if (httpd_query_key_value(buf, "new", param_n, sizeof(param_n)) == ESP_OK) {
                 char* dec = urldecode(param_n); strncpy(new_path, dec, sizeof(new_path)); free(dec);
            }
        }
        free(buf);
    }
    
    if (rename(old_path, new_path) == 0) httpd_resp_sendstr(req, "Renomeado");
    else httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erro");
    return ESP_OK;
}

static esp_err_t api_download_handler(httpd_req_t *req) {
    char file_path[256] = "";
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            char param[256];
            if (httpd_query_key_value(buf, "path", param, sizeof(param)) == ESP_OK) {
                char* dec = urldecode(param); strncpy(file_path, dec, sizeof(file_path)); free(dec);
            }
        }
        free(buf);
    }

    FILE *fp = fopen(file_path, "rb");
    if (!fp) { httpd_resp_send_404(req); return ESP_OK; }

    const char *ext = strrchr(file_path, '.');
    if (ext) {
        if (strcasecmp(ext, ".json") == 0 || strcasecmp(ext, ".txt") == 0) httpd_resp_set_type(req, "text/plain");
        else if (strcasecmp(ext, ".png") == 0) httpd_resp_set_type(req, "image/png");
        else if (strcasecmp(ext, ".jpg") == 0) httpd_resp_set_type(req, "image/jpeg");
        else httpd_resp_set_type(req, "application/octet-stream"); 
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    char *http_buf = (char *)malloc(32768);
    if (http_buf) {
        size_t len;
        while ((len = fread(http_buf, 1, 32768, fp)) > 0) {
            httpd_resp_send_chunk(req, http_buf, len);
            vTaskDelay(1);
        }
        httpd_resp_send_chunk(req, NULL, 0); 
        free(http_buf);
    }
    fclose(fp);
    return ESP_OK;
}

static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 15;
    config.core_id = 0; 
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        
        // CORREÇÃO: Declaração das rotas nativas aceitas pelo C++
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

        httpd_uri_t uri_rename = { .uri = "/api/rename", .method = HTTP_POST, .handler = api_rename_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_rename);

        httpd_uri_t uri_download = { .uri = "/api/download", .method = HTTP_GET, .handler = api_download_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_download);
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
    strcpy((char *)wifi_config.ap.password, "12345678"); 
    wifi_config.ap.ssid_len = strlen("WatchOS-Explorer");
    wifi_config.ap.channel = 6;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
}

static void build_ui() {
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);

    lv_obj_t * title = lv_label_create(scr);
    lv_label_set_text(title, LV_SYMBOL_WIFI " Servidor Web Ativo");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t * info_box = lv_obj_create(scr);
    lv_obj_set_size(info_box, 300, 160);
    lv_obj_align(info_box, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(info_box, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(info_box, lv_color_hex(0x007BFF), 0);
    lv_obj_set_style_border_width(info_box, 2, 0);

    lbl_status = lv_label_create(info_box);
    lv_label_set_text(lbl_status, "Conecte-se a rede Wi-Fi:\n\nSSID: WatchOS-Explorer\nSenha: 12345678\n\nAcesse no Navegador:\nhttp://192.168.4.1");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00FF00), 0); 
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl_status);

    lv_obj_t * btn_exit = lv_btn_create(scr);
    lv_obj_set_size(btn_exit, 240, 60);
    lv_obj_align(btn_exit, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0xCC0000), 0); 
    lv_obj_set_style_radius(btn_exit, 30, 0);
    
    lv_obj_t * lbl_exit = lv_label_create(btn_exit);
    lv_label_set_text(lbl_exit, LV_SYMBOL_POWER " Encerrar e Voltar");
    lv_obj_set_style_text_font(lbl_exit, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_exit);
    
    lv_obj_add_event_cb(btn_exit, [](lv_event_t *e){ return_to_factory(); }, LV_EVENT_CLICKED, NULL);
}

extern "C" void app_main(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    nvs_flash_init();
    bsp_display_start();
    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();
    vTaskDelay(pdMS_TO_TICKS(100)); 
    bsp_display_brightness_set(80);

    wifi_init_softap();
    start_webserver();

    vTaskDelay(pdMS_TO_TICKS(1000));
    SdUsbManager::get_instance().init_local_storage();
    
    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) return_to_factory();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}