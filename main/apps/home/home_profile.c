#include "apps/home/home_profile.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "home_profile";

#define NVS_NAMESPACE "home_profile"
#ifndef SPIFFS_BASE
#define SPIFFS_BASE "/storage"
#endif
// 换名以弃用旧版 64×64 RGB565 头像(avatar.bin,8192B)—— 旧文件因尺寸校验
// 不再被识别,重新上传一次即可;文件留在 SPIFFS 里无害。
#define AVATAR_PATH SPIFFS_BASE "/portrait.bin"
static const char *const s_avatar_paths[] = {
    SPIFFS_BASE "/portrait-a.bin", SPIFFS_BASE "/portrait-b.bin"
};
static bool s_avatar_loaded;
static int s_avatar_bank = -1;
static FILE *s_avatar_stream;
static size_t s_avatar_written, s_avatar_expected;
static int s_avatar_upload_bank;
#define SHARE_TMP_PATH SPIFFS_BASE "/share.tmp"
#define SHARE_PATH SPIFFS_BASE "/share.jpg"
#define SHARE_OLD_PATH SPIFFS_BASE "/share.bin"
typedef struct { uint8_t slot; char bank; } Card;
static Card s_cards[HOME_SHARE_MAX_IMAGES];
static uint8_t s_card_count;
static bool s_cards_loaded;
static FILE *s_card_stream;
static size_t s_card_written;
static uint8_t s_upload_slot;
static char s_upload_bank;
static char s_upload_path[HOME_SHARE_PATH_MAX];

// 资料读写在 httpd 任务与首页任务间并发,用临界区保护结构体与版本号。
// 单核 C3 上临界区会阻断其他任务,写路径(NVS/SPIFFS IO)必须放在临界区外。
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static home_profile_t s_profile;
static uint32_t s_version = 1;

static bool utf8_len_ok(const char *s, size_t cap) {
    return s && strnlen(s, cap) < cap; /* strlen 已含结尾 0 之外的全部字节 */
}

static bool coordinate_valid(const char *s, double min, double max) {
    if (!s || !*s || strnlen(s, HOME_COORD_MAX) >= HOME_COORD_MAX) return false;
    const char *p = s;
    if (*p == '-' || *p == '+') ++p;
    bool digit = false, dot = false;
    for (; *p; ++p) {
        if (*p >= '0' && *p <= '9') digit = true;
        else if (*p == '.' && !dot) dot = true;
        else return false;
    }
    if (!digit) return false;
    const double n = strtod(s, NULL);
    return isfinite(n) && n >= min && n <= max;
}

bool home_profile_coordinates_valid(const char *lat, const char *lon) {
    return lat && lon && ((!lat[0] && !lon[0]) ||
        (coordinate_valid(lat, -90, 90) && coordinate_valid(lon, -180, 180)));
}

void home_profile_init(void) {
    s_cards_loaded = false;
    s_avatar_loaded = false;
    memset(&s_profile, 0, sizeof(s_profile));
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No profile stored yet, using empty profile");
        return;
    }
    size_t len;
    len = sizeof(s_profile.name);
    nvs_get_str(handle, "name", s_profile.name, &len);
    len = sizeof(s_profile.signature);
    nvs_get_str(handle, "signature", s_profile.signature, &len);
    len = sizeof(s_profile.city);
    nvs_get_str(handle, "city", s_profile.city, &len);
    len = sizeof(s_profile.address);
    nvs_get_str(handle, "address", s_profile.address, &len);
    len = sizeof(s_profile.lat);
    nvs_get_str(handle, "lat", s_profile.lat, &len);
    len = sizeof(s_profile.lon);
    nvs_get_str(handle, "lon", s_profile.lon, &len);
    len = sizeof(s_profile.region);
    nvs_get_str(handle, "region", s_profile.region, &len);
    len = sizeof(s_profile.radio_city);
    nvs_get_str(handle, "radio_city", s_profile.radio_city, &len);
    len = sizeof(s_profile.country_code);
    nvs_get_str(handle, "country_code", s_profile.country_code, &len);
    nvs_close(handle);
    ESP_LOGI(TAG, "Profile loaded: name='%s' city='%s' lat='%s' lon='%s'",
             s_profile.name, s_profile.city, s_profile.lat, s_profile.lon);
}

void home_profile_get(home_profile_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_mux);
    *out = s_profile;
    portEXIT_CRITICAL(&s_mux);
}

uint32_t home_profile_version(void) {
    portENTER_CRITICAL(&s_mux);
    uint32_t v = s_version;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

esp_err_t home_profile_set_text(const home_profile_t *in) {
    if (!in) return ESP_ERR_INVALID_ARG;
    if (!utf8_len_ok(in->name, HOME_NAME_MAX) ||
        !utf8_len_ok(in->signature, HOME_SIGNATURE_MAX) ||
        !utf8_len_ok(in->city, HOME_CITY_MAX) ||
        !utf8_len_ok(in->address, HOME_ADDRESS_MAX) ||
        !utf8_len_ok(in->lat, HOME_COORD_MAX) ||
        !utf8_len_ok(in->lon, HOME_COORD_MAX) ||
        !utf8_len_ok(in->region, HOME_REGION_MAX) ||
        !utf8_len_ok(in->radio_city, HOME_CITY_MAX) ||
        !utf8_len_ok(in->country_code, sizeof(in->country_code))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!home_profile_coordinates_valid(in->lat, in->lon) ||
        (in->country_code[0] && strcmp(in->country_code, "CN") &&
         strcmp(in->country_code, "HK") && strcmp(in->country_code, "MO") &&
         strcmp(in->country_code, "TW"))) return ESP_ERR_INVALID_ARG;

    // 先持久化再上屏:NVS 写失败时保持旧资料,UI 永远与存储一致。
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    do {
        if ((err = nvs_set_str(handle, "name", in->name)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "signature", in->signature)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "city", in->city)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "address", in->address)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "lat", in->lat)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "lon", in->lon)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "region", in->region)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "radio_city", in->radio_city)) != ESP_OK) break;
        if ((err = nvs_set_str(handle, "country_code", in->country_code)) != ESP_OK) break;
        err = nvs_commit(handle);
    } while (false);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Profile save failed: %s", esp_err_to_name(err));
        return err;
    }

    portENTER_CRITICAL(&s_mux);
    s_profile = *in;
    s_version++;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Profile updated: name='%s' city='%s'", in->name, in->city);
    return ESP_OK;
}

// storage 分区若残留其他固件(常见:刷过小智,其 OTA/资源分区覆盖 0x420000 起的
// 区域)或从未初始化,SPIFFS 挂载必失败,头像/名片永远存不进去。分区只存本机
// 头像与名片,可由手机重新上传,因此挂载失败一律重新格式化。整片擦除约 10~15s,
// 开机由 home_profile_storage_prepare() 在后台完成,不再落在手机的 HTTP 请求里。
static StaticSemaphore_t s_storage_lock_buf;
static SemaphoreHandle_t s_storage_lock;
static bool s_storage_mounted, s_storage_preparing, s_storage_deferred;

static SemaphoreHandle_t storage_lock(void) {
    portENTER_CRITICAL(&s_mux);
    if (!s_storage_lock) s_storage_lock = xSemaphoreCreateMutexStatic(&s_storage_lock_buf);
    SemaphoreHandle_t lock = s_storage_lock;
    portEXIT_CRITICAL(&s_mux);
    return lock;
}

// 仅用于日志:说明被覆盖的是什么,便于社区反馈定位。
static const char *storage_contents(const esp_partition_t *partition) {
    uint8_t head[16];
    if (esp_partition_read(partition, 0, head, sizeof(head)) != ESP_OK) return "unreadable";
    if (head[0] == 0xe9) return "firmware image";
    for (size_t i = 0; i < sizeof(head); ++i)
        if (head[i] != 0xff) return "foreign data";
    return "blank";
}

// 格式化后旧名片文件已不存在;清空顺序索引,避免相册列出打不开的条目。
// 头像索引无需处理:文件缺失即视为无头像,下次上传写另一组并重新提交。
static void storage_forget_files(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, "cards_v1", "") == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_mux);
    s_avatar_loaded = false;
    s_cards_loaded = false;
    s_version++;
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t mount_locked(void) {
    if (s_storage_mounted) return ESP_OK;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_FAIL) {
        // A single failed mount can be a transient read glitch; formatting
        // erases avatars and cards, so only format after a second failure.
        vTaskDelay(pdMS_TO_TICKS(50));
        err = esp_vfs_spiffs_register(&conf);
    }
    if (err == ESP_FAIL) {
        const esp_partition_t *partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
        if (!partition) return ESP_ERR_NOT_FOUND;
        const char *contents = storage_contents(partition);
        (void)contents;
        ESP_LOGW(TAG, "SPIFFS mount failed (storage holds %s); formatting", contents);
        conf.format_if_mount_failed = true;
        err = esp_vfs_spiffs_register(&conf);
        if (err == ESP_OK) storage_forget_files();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS unavailable: %s", esp_err_to_name(err));
        return err;
    }
    s_storage_mounted = true;
    return ESP_OK;
}

static esp_err_t ensure_spiffs(void) {
    if (s_storage_mounted) return ESP_OK;
    SemaphoreHandle_t lock = storage_lock();
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = mount_locked();
    xSemaphoreGive(lock);
    return err;
}

// 首页显示路径不等待开机格式化:格式化完成时 s_version 递增,首页会重新加载。
static bool storage_busy(void) {
    portENTER_CRITICAL(&s_mux);
    bool busy = s_storage_preparing;
    if (busy) s_storage_deferred = true;
    portEXIT_CRITICAL(&s_mux);
    return busy;
}

void home_profile_storage_prepare(void) {
    portENTER_CRITICAL(&s_mux);
    s_storage_preparing = true;
    portEXIT_CRITICAL(&s_mux);
    ensure_spiffs();
    portENTER_CRITICAL(&s_mux);
    s_storage_preparing = false;
    if (s_storage_deferred) s_version++;
    s_storage_deferred = false;
    portEXIT_CRITICAL(&s_mux);
}

static esp_err_t avatar_load(void) {
    esp_err_t err = ensure_spiffs();
    if (err != ESP_OK || s_avatar_loaded) return err;
    nvs_handle_t h;
    char bank[2] = {0};
    size_t n = sizeof(bank);
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_str(h, "portrait_v2", bank, &n);
        nvs_close(h);
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NVS_NOT_FOUND) s_avatar_bank = -1;
    else if (err != ESP_OK) return err;
    else if ((bank[0] != 'a' && bank[0] != 'b') || bank[1]) return ESP_ERR_INVALID_STATE;
    else s_avatar_bank = bank[0] - 'a';
    s_avatar_loaded = true;
    return ESP_OK;
}

static const char *avatar_path(void) {
    return s_avatar_bank < 0 ? AVATAR_PATH : s_avatar_paths[s_avatar_bank];
}

bool home_profile_has_avatar(void) {
    if (storage_busy() || avatar_load() != ESP_OK) return false;
    struct stat st;
    return stat(avatar_path(), &st) == 0 &&
        home_portrait_upload_size(st.st_size);
}

esp_err_t home_profile_read_avatar(uint8_t *dst, size_t cap) {
    if (!dst || cap < HOME_AVATAR_MAX_BYTES) return ESP_ERR_INVALID_ARG;
    esp_err_t err = avatar_load();
    if (err != ESP_OK) return err;
    struct stat st;
    if (stat(avatar_path(), &st) != 0) return ESP_ERR_NOT_FOUND;
    if (!home_portrait_upload_size(st.st_size))
        return ESP_ERR_INVALID_SIZE;
    FILE *fp = fopen(avatar_path(), "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;
    size_t got = 0;
    if (st.st_size == HOME_AVATAR_COLOR_BYTES || st.st_size == HOME_AVATAR_BYTES) {
        got = fread(dst, 1, st.st_size, fp);
        fclose(fp);
        return got == (size_t)st.st_size && home_portrait_valid_header(dst) ? ESP_OK : ESP_FAIL;
    }
    memset(dst, 0xff, HOME_AVATAR_BYTES);
    home_portrait_init_header(dst, 0);
    uint8_t *tones = dst + HOME_PORTRAIT_HEADER_BYTES;
    if (st.st_size == HOME_AVATAR_V2_BYTES) got = fread(tones, 1, HOME_AVATAR_V2_BYTES, fp);
    else {
        // Preserve the visible rows of existing 96x150 portraits without changing their pixels.
        for (int y = 0; y < HOME_PORTRAIT_H; ++y) {
            uint8_t row[HOME_PORTRAIT_W / 8];
            if (fread(row, 1, sizeof(row), fp) != sizeof(row)) break;
            for (int x = 0; x < HOME_PORTRAIT_W; x += 4) {
                uint8_t value = 0;
                for (int i = 0; i < 4; ++i)
                    if (row[(x+i)/8] & (128 >> ((x+i)%8))) value |= 3 << (6-2*i);
                tones[got++] = value;
            }
        }
    }
    fclose(fp);
    if (got != HOME_AVATAR_V2_BYTES) {
        ESP_LOGW(TAG, "Avatar short read: %u/%u", (unsigned)got, HOME_AVATAR_V2_BYTES);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void home_profile_abort_avatar_upload(void) {
    if (s_avatar_stream) {
        fclose(s_avatar_stream);
        s_avatar_stream = NULL;
        remove(s_avatar_paths[s_avatar_upload_bank]);
    }
}

static esp_err_t avatar_save_failure(const char *stage, esp_err_t err, int os_errno) {
    (void)stage;
    (void)os_errno;
    ESP_LOGW(TAG, "Avatar save failed: stage=%s err=%s os_errno=%d",
             stage, esp_err_to_name(err), os_errno);
    return err;
}

esp_err_t home_profile_write_avatar_chunk(const uint8_t *data, size_t len,
                                          bool first, bool last, size_t total_bytes) {
    if (!home_portrait_upload_size(total_bytes) ||
        (!data && len)) return avatar_save_failure("input", ESP_ERR_INVALID_ARG, 0);
    esp_err_t err = ensure_spiffs();
    if (err != ESP_OK) return avatar_save_failure("mount", err, 0);
    err = avatar_load();
    if (err != ESP_OK) return avatar_save_failure("index_read", err, 0);
    if (first) {
        home_profile_abort_avatar_upload();
        s_avatar_upload_bank = s_avatar_bank == 0 ? 1 : 0;
        s_avatar_expected = total_bytes;
        s_avatar_written = 0;
        s_avatar_stream = fopen(s_avatar_paths[s_avatar_upload_bank], "wb");
        if (!s_avatar_stream) return avatar_save_failure("open", ESP_FAIL, errno);
    }
    if (!s_avatar_stream) return avatar_save_failure("stream", ESP_ERR_INVALID_STATE, 0);
    if (total_bytes != s_avatar_expected || len > s_avatar_expected - s_avatar_written) {
        home_profile_abort_avatar_upload();
        return avatar_save_failure("length", ESP_ERR_INVALID_SIZE, 0);
    }
    if (len && fwrite(data, 1, len, s_avatar_stream) != len) {
        const int write_errno = errno;
        home_profile_abort_avatar_upload();
        return avatar_save_failure("write", ESP_FAIL, write_errno);
    }
    s_avatar_written += len;
    if (!last) return ESP_OK;
    if (s_avatar_written != s_avatar_expected) {
        home_profile_abort_avatar_upload();
        return avatar_save_failure("incomplete", ESP_ERR_INVALID_SIZE, 0);
    }
    const int flushed = fflush(s_avatar_stream);
    const int flush_errno = flushed ? errno : 0;
    const int closed = fclose(s_avatar_stream);
    const int close_errno = closed ? errno : 0;
    s_avatar_stream = NULL;
    if (flushed || closed) {
        remove(s_avatar_paths[s_avatar_upload_bank]);
        return avatar_save_failure(flushed ? "flush" : "close", ESP_FAIL,
                                   flushed ? flush_errno : close_errno);
    }
    if (total_bytes == HOME_AVATAR_COLOR_BYTES || total_bytes == HOME_AVATAR_BYTES) {
        uint8_t header[HOME_PORTRAIT_HEADER_BYTES];
        FILE *check = fopen(s_avatar_paths[s_avatar_upload_bank], "rb");
        const bool valid = check && fread(header, 1, sizeof(header), check) == sizeof(header) &&
            home_portrait_valid_header(header);
        if (check) fclose(check);
        if (!valid) {
            remove(s_avatar_paths[s_avatar_upload_bank]);
            return avatar_save_failure("validate", ESP_ERR_INVALID_ARG, 0);
        }
    }
    nvs_handle_t h;
    const char *stage = "index_open";
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char bank[] = {(char)('a' + s_avatar_upload_bank), 0};
        stage = "index_write";
        err = nvs_set_str(h, "portrait_v2", bank);
        if (err == ESP_OK) { stage = "index_commit"; err = nvs_commit(h); }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        remove(s_avatar_paths[s_avatar_upload_bank]);
        return avatar_save_failure(stage, err, 0);
    }
    s_avatar_bank = s_avatar_upload_bank;
    portENTER_CRITICAL(&s_mux);
    s_version++;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "Avatar stored (%u bytes, bank %d)", (unsigned)s_avatar_written, s_avatar_bank);
    return ESP_OK;
}

static bool card_path(Card card, char *path, size_t cap) {
    if (!path || !cap || card.slot >= HOME_SHARE_MAX_IMAGES) return false;
    int n = card.bank == 'l' ? snprintf(path, cap, "%s", SHARE_PATH) :
        snprintf(path, cap, SPIFFS_BASE "/card%u%c.jpg", card.slot, card.bank);
    return n >= 0 && (size_t)n < cap;
}

static esp_err_t cards_load(void) {
    esp_err_t err = ensure_spiffs();
    if (err != ESP_OK || s_cards_loaded) return err;
    char order[HOME_SHARE_MAX_IMAGES * 2 + 1] = {0};
    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t n = sizeof(order);
        err = nvs_get_str(h, "cards_v1", order, &n);
        nvs_close(h);
    }
    Card cards[HOME_SHARE_MAX_IMAGES] = {0};
    uint8_t count = 0;
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NVS_NOT_FOUND) {
        struct stat st;
        if (stat(SHARE_PATH, &st) == 0 && st.st_size > 0 && st.st_size <= HOME_SHARE_MAX_BYTES)
            cards[count++] = (Card){0, 'l'}; // Read rc.14's image without copying it.
    } else if (err != ESP_OK) return err;
    else {
        const size_t n = strlen(order);
        if ((n & 1) || n > HOME_SHARE_MAX_IMAGES * 2) return ESP_ERR_INVALID_STATE;
        unsigned used = 0;
        for (size_t i = 0; i < n; i += 2) {
            unsigned slot = (unsigned)(order[i] - '0');
            const char bank = order[i + 1];
            if (slot >= HOME_SHARE_MAX_IMAGES || (used & (1u << slot)) ||
                (bank != 'a' && bank != 'b' && !(bank == 'l' && slot == 0)))
                return ESP_ERR_INVALID_STATE;
            used |= 1u << slot;
            cards[count++] = (Card){(uint8_t)slot, bank};
        }
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_cards, cards, sizeof(cards)); s_card_count = count; s_cards_loaded = true;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

static uint8_t cards_copy(Card *cards) {
    portENTER_CRITICAL(&s_mux);
    memcpy(cards, s_cards, sizeof(s_cards)); uint8_t n = s_card_count;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

static esp_err_t cards_commit(const Card *cards, uint8_t count) {
    char order[HOME_SHARE_MAX_IMAGES * 2 + 1] = {0};
    for (unsigned i = 0; i < count; ++i) {
        order[i * 2] = (char)('0' + cards[i].slot); order[i * 2 + 1] = cards[i].bank;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "cards_v1", order);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&s_mux);
    memset(s_cards, 0, sizeof(s_cards));
    memcpy(s_cards, cards, count * sizeof(Card)); s_card_count = count; ++s_version;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool home_profile_share_path_at(uint8_t position, char *path, size_t cap) {
    if (cards_load() != ESP_OK) return false;
    Card cards[HOME_SHARE_MAX_IMAGES]; const uint8_t n = cards_copy(cards);
    return position < n && card_path(cards[position], path, cap);
}
bool home_profile_share_path_for_slot(uint8_t slot, char *path, size_t cap) {
    if (cards_load() != ESP_OK) return false;
    Card cards[HOME_SHARE_MAX_IMAGES]; const uint8_t n = cards_copy(cards);
    for (unsigned i = 0; i < n; ++i) if (cards[i].slot == slot) return card_path(cards[i], path, cap);
    return false;
}
const char *home_profile_share_image_path(void) {
    static char path[HOME_SHARE_PATH_MAX];
    return home_profile_share_path_at(0, path, sizeof(path)) ? path : SHARE_PATH;
}
bool home_profile_has_share_image(void) {
    char path[HOME_SHARE_PATH_MAX]; struct stat st;
    return home_profile_share_path_at(0, path, sizeof(path)) && stat(path, &st) == 0 &&
        st.st_size > 0 && st.st_size <= HOME_SHARE_MAX_BYTES;
}
esp_err_t home_profile_gallery_get(home_share_gallery_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); esp_err_t err = cards_load();
    if (err != ESP_OK) return err;
    Card cards[HOME_SHARE_MAX_IMAGES]; out->count = cards_copy(cards);
    for (unsigned i = 0; i < out->count; ++i) {
        char path[HOME_SHARE_PATH_MAX]; struct stat st;
        out->slots[i] = cards[i].slot;
        if (card_path(cards[i], path, sizeof(path)) && stat(path, &st) == 0 &&
            st.st_size > 0 && st.st_size <= HOME_SHARE_MAX_BYTES) out->bytes[i] = (uint32_t)st.st_size;
    }
    size_t total, used;
    err = esp_spiffs_info("storage", &total, &used);
    if (err == ESP_OK) out->storage_free = total >= used ? total - used : 0;
    return err;
}

/* Check baseline JPEG geometry without allocating a compressed image buffer.
 * Decode errors still fail safely in the display's bounded TJPGD path. */
static bool card_jpeg_valid(const char *path, size_t size) {
    FILE *fp = fopen(path, "rb"); if (!fp) return false;
    bool valid = false, baseline = false;
    if (size < 24 || fgetc(fp) != 0xff || fgetc(fp) != 0xd8) goto done;
    while (ftell(fp) >= 0 && (size_t)ftell(fp) < size - 2) {
        if (fgetc(fp) != 0xff) break;
        int marker; do { marker = fgetc(fp); } while (marker == 0xff);
        if (marker == EOF || marker == 0 || marker == 0xd9) break;
        int hi = fgetc(fp), lo = fgetc(fp);
        if (hi < 0 || lo < 0) break;
        unsigned len = ((unsigned)hi << 8) | (unsigned)lo;
        long start = ftell(fp);
        if (len < 2 || start < 0 || (size_t)start + len - 2 > size) break;
        if (marker == 0xc0) {
            unsigned char dimensions[6];
            if (len < 8 || fread(dimensions, 1, 6, fp) != 6) break;
            baseline = dimensions[0] == 8 && dimensions[1] == (HOME_SHARE_H >> 8) &&
                dimensions[2] == (HOME_SHARE_H & 255) && dimensions[3] == (HOME_SHARE_W >> 8) &&
                dimensions[4] == (HOME_SHARE_W & 255) && (dimensions[5] == 1 || dimensions[5] == 3);
            if (!baseline) break;
        } else if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) break;
        if (marker == 0xda) {
            if (baseline && fseek(fp, -2, SEEK_END) == 0)
                valid = fgetc(fp) == 0xff && fgetc(fp) == 0xd9;
            break;
        }
        if (fseek(fp, start + len - 2, SEEK_SET) != 0) break;
    }
done:
    fclose(fp); return valid;
}

void home_profile_abort_card_upload(void) {
    if (s_card_stream) { fclose(s_card_stream); s_card_stream = NULL; }
    if (s_upload_path[0]) remove(s_upload_path);
    s_upload_path[0] = 0; s_card_written = 0;
}
esp_err_t home_profile_write_card_chunk(uint8_t slot, const uint8_t *data, size_t len,
                                       bool first, bool last) {
    if (slot >= HOME_SHARE_MAX_IMAGES || (len && !data)) return ESP_ERR_INVALID_ARG;
    if (cards_load() != ESP_OK) return ESP_FAIL;
    Card cards[HOME_SHARE_MAX_IMAGES]; uint8_t count = cards_copy(cards);
    int at = -1;
    for (unsigned i = 0; i < count; ++i) if (cards[i].slot == slot) at = (int)i;
    if (first) {
        home_profile_abort_card_upload();
        if (at < 0 && count >= HOME_SHARE_MAX_IMAGES) return ESP_ERR_INVALID_SIZE;
        size_t total, used;
        if (esp_spiffs_info("storage", &total, &used) != ESP_OK || total < used ||
            total - used < HOME_SHARE_STORAGE_RESERVE + HOME_SHARE_MAX_BYTES) return ESP_ERR_NO_MEM;
        s_upload_slot = slot;
        s_upload_bank = at >= 0 && cards[at].bank == 'a' ? 'b' : 'a';
        if (!card_path((Card){slot, s_upload_bank}, s_upload_path, sizeof(s_upload_path))) return ESP_FAIL;
        s_card_stream = fopen(s_upload_path, "wb");
        if (!s_card_stream) { s_upload_path[0] = 0; return ESP_FAIL; }
    }
    if (!s_card_stream || s_upload_slot != slot) return ESP_ERR_INVALID_STATE;
    if (len > HOME_SHARE_MAX_BYTES - s_card_written) {
        home_profile_abort_card_upload(); return ESP_ERR_INVALID_SIZE;
    }
    if (len && fwrite(data, 1, len, s_card_stream) != len) {
        home_profile_abort_card_upload(); return ESP_FAIL;
    }
    s_card_written += len;
    if (!last) return ESP_OK;
    int close_err = fclose(s_card_stream); s_card_stream = NULL;
    if (close_err || !card_jpeg_valid(s_upload_path, s_card_written)) {
        home_profile_abort_card_upload(); return ESP_ERR_INVALID_ARG;
    }
    Card old = {0};
    if (at >= 0) old = cards[at]; else at = count++;
    cards[at] = (Card){slot, s_upload_bank};
    esp_err_t err = cards_commit(cards, count);
    if (err != ESP_OK) { home_profile_abort_card_upload(); return err; }
    s_upload_path[0] = 0; // Committed file is now live: never remove it in abort.
    if (old.bank) { char path[HOME_SHARE_PATH_MAX]; card_path(old, path, sizeof(path)); remove(path); }
    remove(SHARE_TMP_PATH); remove(SHARE_OLD_PATH);
    ESP_LOGI(TAG, "Card slot %u stored: %u bytes; count=%u", slot, (unsigned)s_card_written, count);
    return ESP_OK;
}
esp_err_t home_profile_write_share_image_chunk(const uint8_t *data, size_t len, bool first, bool last) {
    return home_profile_write_card_chunk(0, data, len, first, last);
}
esp_err_t home_profile_gallery_order(const uint8_t *slots, size_t count) {
    if (count > HOME_SHARE_MAX_IMAGES || (count && !slots)) return ESP_ERR_INVALID_ARG;
    if (cards_load() != ESP_OK || s_card_stream) return ESP_ERR_INVALID_STATE;
    Card old[HOME_SHARE_MAX_IMAGES], ordered[HOME_SHARE_MAX_IMAGES] = {0};
    uint8_t old_count = cards_copy(old); unsigned keep = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (slots[i] >= HOME_SHARE_MAX_IMAGES || (keep & (1u << slots[i]))) return ESP_ERR_INVALID_ARG;
        bool found = false;
        for (unsigned j = 0; j < old_count; ++j) if (old[j].slot == slots[i]) { ordered[i] = old[j]; found = true; break; }
        if (!found) return ESP_ERR_NOT_FOUND;
        keep |= 1u << slots[i];
    }
    esp_err_t err = cards_commit(ordered, (uint8_t)count);
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < old_count; ++i) if (!(keep & (1u << old[i].slot))) {
        char path[HOME_SHARE_PATH_MAX]; card_path(old[i], path, sizeof(path)); remove(path);
        card_path((Card){old[i].slot, 'a'}, path, sizeof(path)); remove(path);
        card_path((Card){old[i].slot, 'b'}, path, sizeof(path)); remove(path);
    }
    return ESP_OK;
}
