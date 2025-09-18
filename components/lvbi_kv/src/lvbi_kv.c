/**
 * @file lvbi_kv.c
 * @brief Compact KV store implementation (TLV+CRC, SPIFFS/NVS backing).
 * @details
 *   Implements key/value storage with CRC, file-backed on SPIFFS, and NVS snapshot/restore.
 *   Provides typed API for string, int32, float, and factory reset.
 */

#include "lvbi_kv.h"
#include "esp_spiffs.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define DB_PATH    "/spiffs/config.bin"
#define PART_LABEL "spiffs"      /* must match partitions.csv */
#define NVS_NS     "cfg"
#define NVS_KEY    "factory_blob"

#define MAGIC      0x4C564249UL   /* 'L''V''B''I' */
#define MAX_KEY    24
#define MAX_VAL    64
#define REC_SIZE   (1 + 1 + MAX_KEY + MAX_VAL + 2) /* klen vlen key val crc */
#define HEADER_SZ  8                               /* magic|recs|free_off  */

/* -------- record layout (packed) ----------------------------------------- */
typedef struct __attribute__((packed)) {
    uint8_t  klen;
    uint8_t  vlen;
    char     key[MAX_KEY];
    char     val[MAX_VAL];
    uint16_t crc;
} rec_t;

static const char *TAG = "lvbi_kv";

static FILE *fp                 = NULL;
static uint16_t rec_cnt         = 0;
static uint16_t free_off        = HEADER_SZ;
static SemaphoreHandle_t mutex  = NULL;

/**
 * @brief Compute CRC16 for a data buffer.
 *
 * @details
 *   Calculates the CRC16 checksum for the given data buffer.
 *
 * @param[in] d Pointer to data buffer.
 * @param[in] l Length of data buffer.
 * @return CRC16 value.
 */
static inline uint16_t lvbi_kv_crc16(const void *d, size_t l);

/**
 * @brief Write the KV store header to the file.
 *
 * @details
 *   Writes the header structure (magic, record count, free offset) to the file.
 */
static void lvbi_kv_write_header(void);

/**
 * @brief Read the KV store header from the file.
 *
 * @details
 *   Reads the header structure from the file and updates globals.
 *
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t lvbi_kv_read_header(void);

/**
 * @brief Ensure SPIFFS is mounted and ready.
 *
 * @details
 *   Mounts the SPIFFS partition if not already mounted.
 */
static void lvbi_kv_ensure_spiffs(void);

/**
 * @brief Find the latest record for a given key.
 *
 * @details
 *   Searches the KV file for the most recent record matching the key.
 *
 * @param[in]  k   Key name.
 * @param[out] out Pointer to record structure to fill.
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise.
 */
static esp_err_t lvbi_kv_find_latest(const char *k, rec_t *out);

/**
 * @brief Append a new record to the KV store file.
 *
 * @details
 *   Adds a new key/value record to the end of the file.
 *
 * @param[in] k Key name.
 * @param[in] v Value string.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t lvbi_kv_append_record(const char *k, const char *v);

/**
 * @brief Store a blob in NVS for factory reset.
 *
 * @details
 *   Saves a binary blob to NVS under the factory key.
 *
 * @param[in] d Pointer to data.
 * @param[in] l Length of data.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t lvbi_kv_nvs_put_blob(const void *d, size_t l);

/**
 * @brief Retrieve a blob from NVS for factory restore.
 *
 * @details
 *   Loads a binary blob from NVS under the factory key.
 *
 * @param[out] d Pointer to output buffer.
 * @param[in,out] l Pointer to length (input: buffer size, output: actual size).
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
static esp_err_t lvbi_kv_nvs_get_blob(void *d, size_t *l);

/* ---------- file open / format ----------------------------------------- */
esp_err_t lvbi_kv_open(void)
{
    if (!mutex) mutex = xSemaphoreCreateMutex();
    lvbi_kv_ensure_spiffs();

    fp = fopen(DB_PATH, "r+b");
    if (!fp || lvbi_kv_read_header() != ESP_OK) {
        /* create fresh DB */
        if (fp) fclose(fp);
        fp = fopen(DB_PATH, "w+b");
        rec_cnt  = 0;
        free_off = HEADER_SZ;
        lvbi_kv_write_header();
        ESP_LOGI(TAG, "new DB created");
    } else {
        ESP_LOGI(TAG, "DB loaded (%d rec, free_off=0x%X)", rec_cnt, free_off);
    }
    return ESP_OK;
}

void lvbi_kv_close(void) { 
    if (fp) fclose(fp); 
}

/* ---------- string API --------------------------------------------------- */
esp_err_t lvbi_kv_set_str(const char *k, const char *v)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t r = lvbi_kv_append_record(k, v);
    xSemaphoreGive(mutex);
    return r;
}

esp_err_t lvbi_kv_get_str(const char *k, char *out, size_t max)
{
    rec_t rec;
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t r = lvbi_kv_find_latest(k, &rec);
    xSemaphoreGive(mutex);
    if (r == ESP_OK) {
        size_t n = rec.vlen < max ? rec.vlen : max - 1;
        memcpy(out, rec.val, n); out[n] = 0;
    }
    return r;
}


/* ---------- NVS blob backup/restore -------------------------------------- */
esp_err_t lvbi_kv_factory_save(void)
{
    size_t len = free_off;
    void *buf = malloc(len);
    fseek(fp, 0, SEEK_SET); fread(buf, 1, len, fp);
    esp_err_t r = lvbi_kv_nvs_put_blob(buf, len);
    free(buf);
    return r;
}

esp_err_t lvbi_kv_factory_restore(void)
{
    size_t len = 0;
    if (lvbi_kv_nvs_get_blob(NULL, &len) != ESP_OK) return ESP_ERR_NOT_FOUND;
    void *buf = malloc(len);
    lvbi_kv_nvs_get_blob(buf, &len);
    fp = freopen(DB_PATH, "wb", fp);
    fwrite(buf, 1, len, fp); fflush(fp);
    free(buf);
    ESP_LOGW(TAG, "factory config restored - reboot required");
    return ESP_OK;
}


/* ---------- CRC helper --------------------------------------------------- */
static inline uint16_t lvbi_kv_crc16(const void *d, size_t l)
{ 
    return esp_crc16_le(0, d, l); 
}

/* ---------- header I/O --------------------------------------------------- */
static void lvbi_kv_write_header(void)
{
    uint32_t hdr[2] = { MAGIC, ((uint32_t)rec_cnt << 16) | free_off };
    fseek(fp, 0, SEEK_SET);
    fwrite(hdr, 1, HEADER_SZ, fp);
    fflush(fp);
}

static esp_err_t lvbi_kv_read_header(void)
{
    uint32_t hdr[2];
    fseek(fp, 0, SEEK_SET);
    if (fread(hdr, 1, HEADER_SZ, fp) != HEADER_SZ) return ESP_FAIL;
    if (hdr[0] != MAGIC)                return ESP_ERR_INVALID_STATE;
    rec_cnt  = hdr[1] >> 16;
    free_off = hdr[1] & 0xFFFF;
    return ESP_OK;
}

/* ---------- SPIFFS mount once ------------------------------------------- */
static void lvbi_kv_ensure_spiffs(void)
{
    static bool mounted = false;
    if (mounted) return;

    esp_vfs_spiffs_conf_t cfg = {
        .base_path              = "/spiffs",
        .partition_label        = PART_LABEL,
        .max_files              = 4,
        .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&cfg));
    mounted = true;
}

/* ---------- find latest -------------------------------------------------- */
static esp_err_t lvbi_kv_find_latest(const char *k, rec_t *out)
{
    for (int i = rec_cnt - 1; i >= 0; --i) {
        fseek(fp, HEADER_SZ + i * REC_SIZE, SEEK_SET);
        fread(out, 1, REC_SIZE, fp);
        if (lvbi_kv_crc16(out, REC_SIZE - 2) != out->crc) continue;
        if (strncmp(k, out->key, out->klen) == 0) return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ---------- append record ------------------------------------------------ */
static esp_err_t lvbi_kv_append_record(const char *k, const char *v)
{
    size_t kl = strnlen(k, MAX_KEY);
    size_t vl = strnlen(v, MAX_VAL);
    if (!kl || kl >= MAX_KEY || vl >= MAX_VAL) return ESP_ERR_INVALID_ARG;

    rec_t rec = { .klen = (uint8_t)kl, .vlen = (uint8_t)vl };
    memcpy(rec.key, k, kl);
    memcpy(rec.val, v, vl);
    rec.crc = lvbi_kv_crc16(&rec, REC_SIZE - 2);

    fseek(fp, free_off, SEEK_SET);
    fwrite(&rec, 1, REC_SIZE, fp);   /* crash-safe append */
    rec_cnt++;
    free_off += REC_SIZE;
    lvbi_kv_write_header();
    return ESP_OK;
}

/* ---------- factory snapshot in NVS ------------------------------------- */
static esp_err_t lvbi_kv_nvs_put_blob(const void *d, size_t l)
{
    nvs_handle_t h; ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, NVS_KEY, d, l));
    esp_err_t r = nvs_commit(h); nvs_close(h); return r;
}

static esp_err_t lvbi_kv_nvs_get_blob(void *d, size_t *l)
{
    nvs_handle_t h; esp_err_t r = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (r != ESP_OK) return r;
    r = nvs_get_blob(h, NVS_KEY, d, l); 
    nvs_close(h); 
    return r;
}
