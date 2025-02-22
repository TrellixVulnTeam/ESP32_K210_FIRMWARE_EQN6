// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// -------------------------------------------------
// Modified by LoBo to include the directory support
// -------------------------------------------------

#include "esp_spiffs_lobo.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "esp_image_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <unistd.h>
#include <dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include "esp_vfs.h"
#include "esp_err.h"
#include "esp32/rom/spi_flash.h"
#include "spiffs_api_lobo.h"

#if defined (CONFIG_SPIFFS_USE_MTIME) && defined (CONFIG_SPIFFS_USE_DIR)
_Static_assert(CONFIG_SPIFFS_META_LENGTH >= sizeof(time_t)+sizeof(uint8_t),
        "SPIFFS_META_LENGTH size should be >= sizeof(time_t)+sizeof(uint8_t)");
#elif defined (CONFIG_SPIFFS_USE_MTIME)
_Static_assert(CONFIG_SPIFFS_META_LENGTH >= sizeof(time_t),
        "SPIFFS_META_LENGTH size should be >= sizeof(time_t)");
#elif defined (CONFIG_SPIFFS_USE_DIR)
_Static_assert(CONFIG_SPIFFS_META_LENGTH >= sizeof(uint8_t),
        "SPIFFS_META_LENGTH size should be >= sizeof(uint8_t)");
#endif

/**
 * @brief SPIFFS DIR structure
 */
typedef struct {
    DIR dir;            /*!< VFS DIR struct */
    spiffs_DIR d;       /*!< SPIFFS DIR struct */
    struct dirent e;    /*!< Last open dirent */
    long offset;        /*!< Offset of the current dirent */
    char path[SPIFFS_OBJ_NAME_LEN]; /*!< Requested directory name */
} vfs_spiffs_dir_t;

#if defined (CONFIG_SPIFFS_USE_MTIME) || defined (CONFIG_SPIFFS_USE_DIR)
/**
 * @brief SPIFFS metadata structure
 */
typedef struct {
#ifdef CONFIG_SPIFFS_USE_MTIME
    time_t mtime;   /*!< file modification time */
#endif
#ifdef CONFIG_SPIFFS_USE_DIR
    uint8_t type;   /*!< file type */
#endif
} __attribute__((packed, aligned(1))) vfs_spiffs_meta_t;
#endif

static int vfs_spiffs_open(void* ctx, const char * path, int flags, int mode);
static ssize_t vfs_spiffs_write(void* ctx, int fd, const void * data, size_t size);
static ssize_t vfs_spiffs_read(void* ctx, int fd, void * dst, size_t size);
static int vfs_spiffs_close(void* ctx, int fd);
static off_t vfs_spiffs_lseek(void* ctx, int fd, off_t offset, int mode);
static int vfs_spiffs_fstat(void* ctx, int fd, struct stat * st);
static int vfs_spiffs_stat(void* ctx, const char * path, struct stat * st);
static int vfs_spiffs_unlink(void* ctx, const char *path);
static int vfs_spiffs_link(void* ctx, const char* n1, const char* n2);
static int vfs_spiffs_rename(void* ctx, const char *src, const char *dst);
static DIR* vfs_spiffs_opendir(void* ctx, const char* name);
static int vfs_spiffs_closedir(void* ctx, DIR* pdir);
static struct dirent* vfs_spiffs_readdir(void* ctx, DIR* pdir);
static int vfs_spiffs_readdir_r(void* ctx, DIR* pdir,
                                struct dirent* entry, struct dirent** out_dirent);
static long vfs_spiffs_telldir(void* ctx, DIR* pdir);
static void vfs_spiffs_seekdir(void* ctx, DIR* pdir, long offset);
static int vfs_spiffs_mkdir(void* ctx, const char* name, mode_t mode);
static int vfs_spiffs_rmdir(void* ctx, const char* name);
static void vfs_spiffs_update_meta(spiffs *fs, spiffs_file f, uint8_t type);
static time_t vfs_spiffs_get_mtime(const spiffs_stat* s);

static esp_spiffs_t * _efs[CONFIG_SPIFFS_MAX_PARTITIONS];

static const char* SPIFFS_TAG = "SPIFFS";

static void esp_spiffs_free(esp_spiffs_t ** efs)
{
    esp_spiffs_t * e = *efs;
    if (*efs == NULL) {
        return;
    }
    *efs = NULL;

    if (e->fs) {
        SPIFFS_unmount(e->fs);
        free(e->fs);
    }
    vSemaphoreDelete(e->lock);
    free(e->fds);
    free(e->cache);
    free(e->work);
    free(e);
}

static esp_err_t esp_spiffs_by_label(const char* label, int * index){
    int i;
    esp_spiffs_t * p;
    for (i = 0; i < CONFIG_SPIFFS_MAX_PARTITIONS; i++) {
        p = _efs[i];
        if (p) {
            if (!label && !p->by_label) {
                *index = i;
                return ESP_OK;
            }
            if (label && p->by_label && strncmp(label, p->partition->label, 17) == 0) {
                *index = i;
                return ESP_OK;
            }
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t esp_spiffs_get_empty(int * index){
    int i;
    for (i = 0; i < CONFIG_SPIFFS_MAX_PARTITIONS; i++) {
        if (_efs[i] == NULL) {
            *index = i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t esp_spiffs_init(const esp_vfs_spiffs_conf_t* conf)
{
    int index;
    //find if such partition is already mounted
    if (esp_spiffs_by_label(conf->partition_label, &index) == ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    if (esp_spiffs_get_empty(&index) != ESP_OK) {
        ESP_LOGE(SPIFFS_TAG, "max mounted partitions reached");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t flash_page_size = g_rom_flashchip.page_size;
    uint32_t log_page_size = CONFIG_SPIFFS_PAGE_SIZE;
    if (log_page_size % flash_page_size != 0) {
        ESP_LOGE(SPIFFS_TAG, "SPIFFS_PAGE_SIZE is not multiple of flash chip page size (%d)",
                flash_page_size);
        return ESP_ERR_INVALID_ARG;
    }

    esp_partition_subtype_t subtype = conf->partition_label ?
            ESP_PARTITION_SUBTYPE_ANY : ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      subtype, conf->partition_label);
    if (!partition) {
        ESP_LOGE(SPIFFS_TAG, "spiffs partition could not be found");
        return ESP_ERR_NOT_FOUND;
    }

    if (partition->encrypted) {
        ESP_LOGE(SPIFFS_TAG, "spiffs can not run on encrypted partition");
        return ESP_ERR_INVALID_STATE;
    }

    esp_spiffs_t * efs = malloc(sizeof(esp_spiffs_t));
    if (efs == NULL) {
        ESP_LOGE(SPIFFS_TAG, "esp_spiffs could not be malloced");
        return ESP_ERR_NO_MEM;
    }
    memset(efs, 0, sizeof(esp_spiffs_t));

    efs->cfg.hal_erase_f       = spiffs_api_erase;
    efs->cfg.hal_read_f        = spiffs_api_read;
    efs->cfg.hal_write_f       = spiffs_api_write;
    efs->cfg.log_block_size    = g_rom_flashchip.sector_size;
    efs->cfg.log_page_size     = log_page_size;
    efs->cfg.phys_addr         = 0;
    efs->cfg.phys_erase_block  = g_rom_flashchip.sector_size;
    efs->cfg.phys_size         = partition->size;

    efs->by_label = conf->partition_label != NULL;

    efs->lock = xSemaphoreCreateMutex();
    if (efs->lock == NULL) {
        ESP_LOGE(SPIFFS_TAG, "mutex lock could not be created");
        esp_spiffs_free(&efs);
        return ESP_ERR_NO_MEM;
    }

    efs->fds_sz = conf->max_files * sizeof(spiffs_fd);
    efs->fds = malloc(efs->fds_sz);
    if (efs->fds == NULL) {
        ESP_LOGE(SPIFFS_TAG, "fd buffer could not be malloced");
        esp_spiffs_free(&efs);
        return ESP_ERR_NO_MEM;
    }
    memset(efs->fds, 0, efs->fds_sz);

#if SPIFFS_CACHE
    efs->cache_sz = sizeof(spiffs_cache) + conf->max_files * (sizeof(spiffs_cache_page)
                          + efs->cfg.log_page_size);
    efs->cache = malloc(efs->cache_sz);
    if (efs->cache == NULL) {
        ESP_LOGE(SPIFFS_TAG, "cache buffer could not be malloced");
        esp_spiffs_free(&efs);
        return ESP_ERR_NO_MEM;
    }
    memset(efs->cache, 0, efs->cache_sz);
#endif

    const uint32_t work_sz = efs->cfg.log_page_size * 2;
    efs->work = malloc(work_sz);
    if (efs->work == NULL) {
        ESP_LOGE(SPIFFS_TAG, "work buffer could not be malloced");
        esp_spiffs_free(&efs);
        return ESP_ERR_NO_MEM;
    }
    memset(efs->work, 0, work_sz);

    efs->fs = malloc(sizeof(spiffs));
    if (efs->fs == NULL) {
        ESP_LOGE(SPIFFS_TAG, "spiffs could not be malloced");
        esp_spiffs_free(&efs);
        return ESP_ERR_NO_MEM;
    }
    memset(efs->fs, 0, sizeof(spiffs));

    efs->fs->user_data = (void *)efs;
    efs->partition = partition;

    s32_t res = SPIFFS_mount(efs->fs, &efs->cfg, efs->work, efs->fds, efs->fds_sz,
                            efs->cache, efs->cache_sz, spiffs_api_check);

    if (conf->format_if_mount_failed && res != SPIFFS_OK) {
        ESP_LOGW(SPIFFS_TAG, "mount failed, %i. formatting...", SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        res = SPIFFS_format(efs->fs);
        if (res != SPIFFS_OK) {
            ESP_LOGE(SPIFFS_TAG, "format failed, %i", SPIFFS_errno(efs->fs));
            SPIFFS_clearerr(efs->fs);
            esp_spiffs_free(&efs);
            return ESP_FAIL;
        }
        res = SPIFFS_mount(efs->fs, &efs->cfg, efs->work, efs->fds, efs->fds_sz,
                            efs->cache, efs->cache_sz, spiffs_api_check);
    }
    if (res != SPIFFS_OK) {
        ESP_LOGE(SPIFFS_TAG, "mount failed, %i", SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        esp_spiffs_free(&efs);
        return ESP_FAIL;
    }
    _efs[index] = efs;
    return ESP_OK;
}

// === Global functions ===

bool esp_spiffs_mounted_lobo(const char* partition_label)
{
    int index;
    if (esp_spiffs_by_label(partition_label, &index) != ESP_OK) {
        return false;
    }
    return (SPIFFS_mounted(_efs[index]->fs));
}

esp_err_t esp_spiffs_info_lobo(const char* partition_label, size_t *total_bytes, size_t *used_bytes)
{
    int index;
    if (esp_spiffs_by_label(partition_label, &index) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    SPIFFS_info(_efs[index]->fs, total_bytes, used_bytes);
    return ESP_OK;
}

esp_err_t esp_spiffs_format_lobo(const char* partition_label)
{
    bool partition_was_mounted = false;
    int index;
    /* If the partition is not mounted, need to create SPIFFS structures
     * and mount the partition, unmount, format, delete SPIFFS structures.
     * See SPIFFS wiki for the reason why.
     */
    esp_err_t err = esp_spiffs_by_label(partition_label, &index);
    if (err != ESP_OK) {
        esp_vfs_spiffs_conf_t conf = {
                .format_if_mount_failed = true,
                .partition_label = partition_label,
                .max_files = 1
        };
        err = esp_spiffs_init(&conf);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_spiffs_by_label(partition_label, &index);
        assert(err == ESP_OK && "failed to get index of the partition just mounted");
    } else if (SPIFFS_mounted(_efs[index]->fs)) {
        partition_was_mounted = true;
    }

    SPIFFS_unmount(_efs[index]->fs);

    s32_t res = SPIFFS_format(_efs[index]->fs);
    if (res != SPIFFS_OK) {
        ESP_LOGE(SPIFFS_TAG, "format failed, %i", SPIFFS_errno(_efs[index]->fs));
        SPIFFS_clearerr(_efs[index]->fs);
        /* If the partition was previously mounted, but format failed, don't
         * try to mount the partition back (it will probably fail). On the
         * other hand, if it was not mounted, need to clean up.
         */
        if (!partition_was_mounted) {
            esp_spiffs_free(&_efs[index]);
        }
        return ESP_FAIL;
    }

    if (partition_was_mounted) {
        res = SPIFFS_mount(_efs[index]->fs, &_efs[index]->cfg, _efs[index]->work,
                            _efs[index]->fds, _efs[index]->fds_sz, _efs[index]->cache,
                            _efs[index]->cache_sz, spiffs_api_check);
        if (res != SPIFFS_OK) {
            ESP_LOGE(SPIFFS_TAG, "mount failed, %i", SPIFFS_errno(_efs[index]->fs));
            SPIFFS_clearerr(_efs[index]->fs);
            return ESP_FAIL;
        }
    } else {
        esp_spiffs_free(&_efs[index]);
    }
    return ESP_OK;
}

esp_err_t esp_vfs_spiffs_register_lobo(const esp_vfs_spiffs_conf_t * conf)
{
    assert(conf->base_path);
    const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .write_p = &vfs_spiffs_write,
        .lseek_p = &vfs_spiffs_lseek,
        .read_p = &vfs_spiffs_read,
        .open_p = &vfs_spiffs_open,
        .close_p = &vfs_spiffs_close,
        .fstat_p = &vfs_spiffs_fstat,
        .stat_p = &vfs_spiffs_stat,
        .link_p = &vfs_spiffs_link,
        .unlink_p = &vfs_spiffs_unlink,
        .rename_p = &vfs_spiffs_rename,
        .opendir_p = &vfs_spiffs_opendir,
        .closedir_p = &vfs_spiffs_closedir,
        .readdir_p = &vfs_spiffs_readdir,
        .readdir_r_p = &vfs_spiffs_readdir_r,
        .seekdir_p = &vfs_spiffs_seekdir,
        .telldir_p = &vfs_spiffs_telldir,
        .mkdir_p = &vfs_spiffs_mkdir,
        .rmdir_p = &vfs_spiffs_rmdir
    };

    esp_err_t err = esp_spiffs_init(conf);
    if (err != ESP_OK) {
        return err;
    }

    int index;
    if (esp_spiffs_by_label(conf->partition_label, &index) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    strlcat(_efs[index]->base_path, conf->base_path, ESP_VFS_PATH_MAX + 1);
    err = esp_vfs_register(conf->base_path, &vfs, _efs[index]);
    if (err != ESP_OK) {
        esp_spiffs_free(&_efs[index]);
        return err;
    }

    return ESP_OK;
}

esp_err_t esp_vfs_spiffs_unregister_lobo(const char* partition_label)
{
    int index;
    if (esp_spiffs_by_label(partition_label, &index) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_vfs_unregister(_efs[index]->base_path);
    if (err != ESP_OK) {
        return err;
    }
    esp_spiffs_free(&_efs[index]);
    return ESP_OK;
}

static int spiffs_res_to_errno(s32_t fr)
{
    switch(fr) {
    case SPIFFS_OK :
        return 0;
    case SPIFFS_ERR_NOT_MOUNTED :
        return ENODEV;
    case SPIFFS_ERR_NOT_A_FS :
        return ENODEV;
    case SPIFFS_ERR_FULL :
        return ENOSPC;
    case SPIFFS_ERR_BAD_DESCRIPTOR :
        return EBADF;
    case SPIFFS_ERR_MOUNTED :
        return EEXIST;
    case SPIFFS_ERR_FILE_EXISTS :
        return EEXIST;
    case SPIFFS_ERR_NOT_FOUND :
        return ENOENT;
    case SPIFFS_ERR_NOT_A_FILE :
        return ENOENT;
    case SPIFFS_ERR_DELETED :
        return ENOENT;
    case SPIFFS_ERR_FILE_DELETED :
        return ENOENT;
    case SPIFFS_ERR_NAME_TOO_LONG :
        return ENAMETOOLONG;
    case SPIFFS_ERR_RO_NOT_IMPL :
        return EROFS;
    case SPIFFS_ERR_RO_ABORTED_OPERATION :
        return EROFS;
    default :
        return EIO;
    }
    return ENOTSUP;
}

static int spiffs_mode_conv(int m)
{
    int res = 0;
    int acc_mode = m & O_ACCMODE;
    if (acc_mode == O_RDONLY) {
        res |= SPIFFS_O_RDONLY;
    } else if (acc_mode == O_WRONLY) {
        res |= SPIFFS_O_WRONLY;
    } else if (acc_mode == O_RDWR) {
        res |= SPIFFS_O_RDWR;
    }
    if ((m & O_CREAT) && (m & O_EXCL)) {
        res |= SPIFFS_O_CREAT | SPIFFS_O_EXCL;
    } else if ((m & O_CREAT) && (m & O_TRUNC)) {
        res |= SPIFFS_O_CREAT | SPIFFS_O_TRUNC;
    }
    if (m & O_APPEND) {
        res |= SPIFFS_O_CREAT | SPIFFS_O_APPEND;
    }
    return res;
}

static int vfs_spiffs_open(void* ctx, const char * path, int flags, int mode)
{
    assert(path);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    int spiffs_flags = spiffs_mode_conv(flags);
    int fd = SPIFFS_open(efs->fs, path, spiffs_flags, mode);
    if (fd < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
#ifdef CONFIG_SPIFFS_USE_DIR
    spiffs_stat s;
    int ret = SPIFFS_fstat(efs->fs, fd, &s);
    if (ret == SPIFFS_OK) {
        vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
        if (meta->type == SPIFFS_TYPE_DIR) {
            // It is directory, cannot be opened
            errno = EISDIR;
            ret = SPIFFS_close(efs->fs, fd);
            if (ret < 0) {
                errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
                SPIFFS_clearerr(efs->fs);
            }
            return -1;
        }
    }
#endif
    if (!(spiffs_flags & SPIFFS_RDONLY)) {
        vfs_spiffs_update_meta(efs->fs, fd, SPIFFS_TYPE_FILE);
    }
    return fd;
}

static ssize_t vfs_spiffs_write(void* ctx, int fd, const void * data, size_t size)
{
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    ssize_t res = SPIFFS_write(efs->fs, fd, (void *)data, size);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static ssize_t vfs_spiffs_read(void* ctx, int fd, void * dst, size_t size)
{
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    ssize_t res = SPIFFS_read(efs->fs, fd, dst, size);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static int vfs_spiffs_close(void* ctx, int fd)
{
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    int res = SPIFFS_close(efs->fs, fd);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static off_t vfs_spiffs_lseek(void* ctx, int fd, off_t offset, int mode)
{
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    off_t res = SPIFFS_lseek(efs->fs, fd, offset, mode);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static int vfs_spiffs_fstat(void* ctx, int fd, struct stat * st)
{
    assert(st);
    spiffs_stat s;
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    off_t res = SPIFFS_fstat(efs->fs, fd, &s);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    st->st_size = s.size;
#ifdef CONFIG_SPIFFS_USE_DIR
    vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
    if (meta->type == SPIFFS_TYPE_DIR) st->st_mode = S_IFDIR;
    else st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
#else
    st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
#endif
    st->st_mtime = vfs_spiffs_get_mtime(&s);
    st->st_atime = 0;
    st->st_ctime = 0;
    return res;
}

static int vfs_spiffs_stat(void* ctx, const char * path, struct stat * st)
{
    assert(path);
    assert(st);
    spiffs_stat s;
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    off_t res = SPIFFS_stat(efs->fs, path, &s);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }

    st->st_size = s.size;
#ifdef CONFIG_SPIFFS_USE_DIR
    vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
    if (meta->type == SPIFFS_TYPE_DIR) st->st_mode = S_IFDIR;
    else st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
#else
    st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO;
    st->st_mode |= (s.type == SPIFFS_TYPE_DIR)?S_IFDIR:S_IFREG;
#endif
    st->st_mtime = vfs_spiffs_get_mtime(&s);
    st->st_atime = 0;
    st->st_ctime = 0;
    return res;
}

static int vfs_spiffs_rename(void* ctx, const char *src, const char *dst)
{
    assert(src);
    assert(dst);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    int res = SPIFFS_rename(efs->fs, src, dst);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static int vfs_spiffs_unlink(void* ctx, const char *path)
{
    assert(path);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
#ifdef CONFIG_SPIFFS_USE_DIR
    spiffs_stat s;
    off_t ret = SPIFFS_stat(efs->fs, path, &s);
    if (ret < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
    if (meta->type == SPIFFS_TYPE_DIR) {
        // Directory cannot be unliked (removed)
        errno = EISDIR;
        return -1;
    }
#endif
    int res = SPIFFS_remove(efs->fs, path);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static DIR* vfs_spiffs_opendir(void* ctx, const char* name)
{
    assert(name);
#ifdef CONFIG_SPIFFS_USE_DIR
    if (strcmp(name, "/") != 0) {
        // If not on root, check if path exists and is a directory
        struct stat st;
        if (vfs_spiffs_stat(ctx, name, &st)) {
            // Not found
            errno = ENOENT;
            return NULL;
        }
        if (!S_ISDIR(st.st_mode)) {
            // Not a directory, cannot open
            errno = ENOTDIR;
            return NULL;
        }
    }
#endif
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    vfs_spiffs_dir_t * dir = calloc(1, sizeof(vfs_spiffs_dir_t));
    if (!dir) {
        errno = ENOMEM;
        return NULL;
    }
    if (!SPIFFS_opendir(efs->fs, name, &dir->d)) {
        free(dir);
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return NULL;
    }
    dir->offset = 0;
    strlcpy(dir->path, name, SPIFFS_OBJ_NAME_LEN);
    return (DIR*) dir;
}

static int vfs_spiffs_closedir(void* ctx, DIR* pdir)
{
    assert(pdir);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    vfs_spiffs_dir_t * dir = (vfs_spiffs_dir_t *)pdir;
    int res = SPIFFS_closedir(&dir->d);
    free(dir);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
}

static struct dirent* vfs_spiffs_readdir(void* ctx, DIR* pdir)
{
    assert(pdir);
    vfs_spiffs_dir_t * dir = (vfs_spiffs_dir_t *)pdir;
    static struct dirent* out_dirent;

    int err = vfs_spiffs_readdir_r(ctx, pdir, &dir->e, &out_dirent);
    if (err != 0) {
        errno = err;
        return NULL;
    }
    return out_dirent;
}

static int vfs_spiffs_readdir_r(void* ctx, DIR* pdir, struct dirent* entry,
                                struct dirent** out_dirent)
{
    assert(pdir);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    vfs_spiffs_dir_t * dir = (vfs_spiffs_dir_t *)pdir;
    struct spiffs_dirent out;

    // read directory entry
    if (SPIFFS_readdir(&dir->d, &out) == 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        if (!errno) {
            *out_dirent = NULL;
        }
        return errno;
    }
    const char *item_name = (const char *)out.name;
    const char *out_item_name = (const char *)out.name;
    size_t plen = strlen(dir->path); // directory path length

    // === skip all entries not belonging to the requested directory path ===
    if (plen > 1) {
        // on subdirectory
        while ((strstr(item_name, dir->path) != item_name) || (strlen(item_name) <= plen) || (strchr(item_name+plen+1, '/'))) {
            if (SPIFFS_readdir(&dir->d, &out) == 0) {
                errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
                SPIFFS_clearerr(efs->fs);
                if (!errno) {
                    *out_dirent = NULL;
                }
                return errno;
            }
            item_name = (const char *)out.name;
        }
        out_item_name = item_name + plen + 1;
    }
    else {
        // on root
        while ((strlen(item_name) > 2) && (strchr(item_name+1, '/'))) {
            if (SPIFFS_readdir(&dir->d, &out) == 0) {
                errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
                SPIFFS_clearerr(efs->fs);
                if (!errno) {
                    *out_dirent = NULL;
                }
                return errno;
            }
            item_name = (const char *)out.name;
        }
        out_item_name = item_name + plen;
    }
#ifdef CONFIG_SPIFFS_USE_DIR
    // Get file stat, used for setting file type in dirent entry
    spiffs_stat s = {0};
    off_t ret = SPIFFS_stat(efs->fs, item_name, &s);
    if (ret < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return errno;
    }
#endif

    entry->d_ino = 0;
#ifdef CONFIG_SPIFFS_USE_DIR
    vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
    if (meta->type == SPIFFS_TYPE_DIR) entry->d_type = DT_DIR;
    else entry->d_type = out.type;
#else
    entry->d_type = out.type;
#endif
    snprintf(entry->d_name, SPIFFS_OBJ_NAME_LEN, "%s", out_item_name);
    dir->offset++;
    *out_dirent = entry;
    return 0;
}

static long vfs_spiffs_telldir(void* ctx, DIR* pdir)
{
    assert(pdir);
    vfs_spiffs_dir_t * dir = (vfs_spiffs_dir_t *)pdir;
    return dir->offset;
}

static void vfs_spiffs_seekdir(void* ctx, DIR* pdir, long offset)
{
    assert(pdir);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;
    vfs_spiffs_dir_t * dir = (vfs_spiffs_dir_t *)pdir;
    struct spiffs_dirent tmp;
    if (offset < dir->offset) {
        //rewind dir
        SPIFFS_closedir(&dir->d);
        if (!SPIFFS_opendir(efs->fs, NULL, &dir->d)) {
            errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
            SPIFFS_clearerr(efs->fs);
            return;
        }
        dir->offset = 0;
    }
    while (dir->offset < offset) {
        if (SPIFFS_readdir(&dir->d, &tmp) == 0) {
            errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
            SPIFFS_clearerr(efs->fs);
            return;
        }
        size_t plen = strlen(dir->path);
        if (plen > 1) {
            if (strncasecmp(dir->path, (const char *)tmp.name, plen) || tmp.name[plen] != '/' || !tmp.name[plen+1]) {
                continue;
            }
        }
        dir->offset++;
    }
}

static int vfs_spiffs_mkdir(void* ctx, const char* name, mode_t mode)
{
#ifdef CONFIG_SPIFFS_USE_DIR
    assert(name);
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;

    int fd = SPIFFS_open(efs->fs, name, SPIFFS_CREAT | SPIFFS_WRONLY, 0);
    if (fd < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    vfs_spiffs_update_meta(efs->fs, fd, SPIFFS_TYPE_DIR);

    if (SPIFFS_close(efs->fs, fd) < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return 0;
#else
    errno = ENOTSUP;
    return -1;
#endif
}

static int vfs_spiffs_rmdir(void* ctx, const char* name)
{
#ifdef CONFIG_SPIFFS_USE_DIR
    assert(name);
    spiffs_stat s;
    esp_spiffs_t * efs = (esp_spiffs_t *)ctx;

    off_t ret = SPIFFS_stat(efs->fs, name, &s);
    if (ret < 0) {
        // Directory name not found
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        // return success, as it is acctualy "removed"
        return 0;
    }

    vfs_spiffs_meta_t * meta = (vfs_spiffs_meta_t *)&s.meta;
    if (meta->type != SPIFFS_TYPE_DIR) {
        // not a directory
        errno = ENOTDIR;
        return -1;
    }

    // Check if  directory is empty
    int nument = 0;
    char npath[SPIFFS_OBJ_NAME_LEN+8];
    sprintf(npath, efs->base_path);
    strlcat(npath, name, SPIFFS_OBJ_NAME_LEN);
    DIR *dir = opendir(npath);
    if (dir) {
        struct dirent *ent;
        // Read directory entries
        while ((ent = readdir(dir)) != NULL) {
            nument++;
        }
    }
    else {
        errno = ENOTDIR;
        return -1;
    }
    closedir(dir);

    if (nument > 0) {
        // Directory not empty, cannot remove
        errno = ENOTEMPTY;
        return -1;
    }

    int res = SPIFFS_remove(efs->fs, name);
    if (res < 0) {
        errno = spiffs_res_to_errno(SPIFFS_errno(efs->fs));
        SPIFFS_clearerr(efs->fs);
        return -1;
    }
    return res;
#else
    errno = ENOTSUP;
    return -1;
#endif
}

static int vfs_spiffs_link(void* ctx, const char* n1, const char* n2)
{
    errno = ENOTSUP;
    return -1;
}

static void vfs_spiffs_update_meta(spiffs *fs, spiffs_file fd, uint8_t type)
{
#if defined (CONFIG_SPIFFS_USE_MTIME) || defined (CONFIG_SPIFFS_USE_DIR)
    vfs_spiffs_meta_t meta;
#ifdef CONFIG_SPIFFS_USE_MTIME
    meta.mtime = time(NULL);
#endif //CONFIG_SPIFFS_USE_MTIME
#ifdef CONFIG_SPIFFS_USE_DIR
    // Add file type (directory or regular file) to the last byte of metadata
    meta.type = type;
#endif
    int ret = SPIFFS_fupdate_meta(fs, fd, (uint8_t *)&meta);
    if (ret != SPIFFS_OK) {
        ESP_LOGW(SPIFFS_TAG, "Failed to update metadata (%d)", ret);
    }
#endif
}

static time_t vfs_spiffs_get_mtime(const spiffs_stat* s)
{
    time_t t = 0;
#ifdef CONFIG_SPIFFS_USE_MTIME
    vfs_spiffs_meta_t meta;
    memcpy(&meta, s->meta, sizeof(meta));
    t = meta.mtime;
#endif
    return t;
}
