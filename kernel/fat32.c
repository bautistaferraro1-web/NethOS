#include <stdint.h>

#include "fat32.h"
#include "block.h"
#include "partition.h"
#include "console.h"
#include "cpu.h"

#define FAT_SECTOR_MAX 4096u
#define FAT_OPEN_MAX   32u
#define FAT_EOC_MIN    0x0FFFFFF8u
#define FAT_BAD        0x0FFFFFF7u
#define FAT_NAME_MAX   13u
#define FAT_O_ACCMODE  3
#define FAT_O_WRONLY   1
#define FAT_O_RDWR     2
#define FAT_O_CREAT    0100
#define FAT_O_EXCL     0200
#define FAT_O_TRUNC    01000
#define FAT_O_APPEND   02000

struct fat_volume {
    int mounted;
    uint32_t device;
    uint32_t sector_size;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fats;
    uint32_t fat_size;
    uint32_t root_cluster;
    uint32_t cluster_count;
    uint64_t start_lba;
    uint64_t partition_sectors;
    uint64_t total_sectors;
    uint64_t fat_start;
    uint64_t data_start;
    uint32_t active_fat;
    int fat_mirroring;
    uint32_t fsinfo_sector;
    uint32_t backup_boot_sector;
    uint32_t free_count;
    uint32_t next_free;
    int fsinfo_valid;
};

struct fat_node {
    uint32_t cluster;
    uint32_t size;
    uint8_t attr;
    uint64_t entry_lba;
    uint16_t entry_offset;
    uint32_t parent_cluster;
};

struct fat_handle {
    struct file file;
    int used;
    int flags;
    struct fat_node node;
};

static struct fat_volume vol;
static struct fat_handle handles[FAT_OPEN_MAX];

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int fsinfo_signatures_valid(const uint8_t *b)
{
    return le32(b) == 0x41615252u &&
           le32(b + 484) == 0x61417272u &&
           le32(b + 508) == 0xAA550000u;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

static void zero_bytes(uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = 0;
}

static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int name_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((uint8_t)*a) != ascii_lower((uint8_t)*b))
            return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int read_sector(uint64_t relative, uint8_t *buffer)
{
    if (!vol.mounted || relative >= vol.total_sectors)
        return -1;

    if (vol.start_lba > UINT64_MAX - relative)
        return -1;

    return block_read(vol.device, vol.start_lba + relative,
                      1, buffer);
}

static int valid_cluster(uint32_t cluster)
{
    return cluster >= 2 && cluster <= vol.cluster_count + 1;
}

static int cluster_sector(uint32_t cluster, uint32_t sector,
                           uint64_t *relative)
{
    if (!valid_cluster(cluster) ||
        sector >= vol.sectors_per_cluster)
        return -1;

    uint64_t base = vol.data_start
                  + (uint64_t)(cluster - 2) * vol.sectors_per_cluster;

    if (base >= vol.total_sectors ||
        sector >= vol.total_sectors - base)
        return -1;

    *relative = base + sector;
    return 0;
}

/*
 *  0 = siguiente cluster valido
 *  1 = fin de cadena
 * -1 = error de lectura o cadena corrupta
 */
static int next_cluster(uint32_t cluster, uint32_t *next)
{
    if (!valid_cluster(cluster) || !next)
        return -1;

    uint64_t offset = (uint64_t)cluster * 4;
    uint64_t fat_bytes = (uint64_t)vol.fat_size * vol.sector_size;

    if (offset > fat_bytes || fat_bytes - offset < 4)
        return -1;

    uint64_t sector_index = offset / vol.sector_size;
    uint32_t byte_index = (uint32_t)(offset % vol.sector_size);
    uint8_t sector[FAT_SECTOR_MAX];

    if (sector_index >= vol.fat_size ||
        read_sector(vol.fat_start + sector_index, sector) != 0)
        return -1;

    uint32_t value = le32(sector + byte_index) & 0x0FFFFFFFu;

    if (value >= FAT_EOC_MIN) {
        *next = value;
        return 1;
    }

    if (value == 0 || value == 1 ||
        value == FAT_BAD || !valid_cluster(value))
        return -1;

    *next = value;
    return 0;
}

static void short_name(const uint8_t *entry, char out[FAT_NAME_MAX])
{
    uint32_t n = 0;
    uint32_t base_end = 8;

    while (base_end && entry[base_end - 1] == ' ')
        base_end--;

    for (uint32_t i = 0; i < base_end; i++) {
        uint8_t c = entry[i];
        if (i == 0 && c == 0x05)
            c = 0xE5;
        out[n++] = (c >= 32 && c <= 126) ? (char)c : '?';
    }

    uint32_t ext_end = 11;
    while (ext_end > 8 && entry[ext_end - 1] == ' ')
        ext_end--;

    if (ext_end > 8) {
        out[n++] = '.';

        for (uint32_t i = 8; i < ext_end; i++) {
            uint8_t c = entry[i];
            out[n++] = (c >= 32 && c <= 126) ? (char)c : '?';
        }
    }

    out[n] = 0;
}

static int hidden_entry(const uint8_t *entry)
{
    if (entry[0] == 0xE5)
        return 1;

    /* Long filename, etiqueta de volumen, o entrada borrada. */
    if (entry[11] == 0x0F || (entry[11] & 0x08))
        return 1;

    return 0;
}

static void node_from_entry(const uint8_t *entry, struct fat_node *node)
{
    node->cluster = ((uint32_t)le16(entry + 20) << 16)
                  | le16(entry + 26);
    node->cluster &= 0x0FFFFFFFu;
    node->size = le32(entry + 28);
    node->attr = entry[11];
}

/* Devuelve 1 si encuentra una entrada, 0 si no existe, -1 si hay error. */
static int find_child(uint32_t directory, const char *wanted,
                      struct fat_node *result)
{
    uint8_t sector[FAT_SECTOR_MAX];
    uint32_t cluster = directory;

    for (uint32_t step = 0; step < vol.cluster_count; step++) {
        if (!valid_cluster(cluster))
            return -1;

        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            uint64_t lba;

            if (cluster_sector(cluster, s, &lba) != 0 ||
                read_sector(lba, sector) != 0)
                return -1;

            for (uint32_t off = 0; off < vol.sector_size; off += 32) {
                const uint8_t *e = sector + off;

                if (e[0] == 0)
                    return 0;

                if (hidden_entry(e))
                    continue;

                char name[FAT_NAME_MAX];
                short_name(e, name);

                if (name_equal(name, ".") || name_equal(name, ".."))
                    continue;

                if (name_equal(name, wanted)) {
                    node_from_entry(e, result);
                    result->entry_lba = lba;
                    result->entry_offset = (uint16_t)off;
                    result->parent_cluster = directory;
                    return 1;
                }
            }
        }

        uint32_t next;
        int rc = next_cluster(cluster, &next);

        if (rc == 1)
            return 0;
        if (rc != 0)
            return -1;

        cluster = next;
    }

    return -1;
}

/* Convierte "/disk/FOO.TXT" en la ruta relativa a la raiz FAT32. */
static int disk_path(const char *path, const char **relative)
{
    while (*path == '/')
        path++;

    if (!name_equal(path, "disk") &&
        !(path[0] && ascii_lower((uint8_t)path[0]) == 'd' &&
          ascii_lower((uint8_t)path[1]) == 'i' &&
          ascii_lower((uint8_t)path[2]) == 's' &&
          ascii_lower((uint8_t)path[3]) == 'k' &&
          (path[4] == '/' || path[4] == 0)))
        return 0;

    /* name_equal(path,"disk") cubre la raiz; los subpaths requieren limite. */
    if (!name_equal(path, "disk") &&
        !(path[4] == '/' || path[4] == 0))
        return 0;

    path += 4;
    while (*path == '/')
        path++;

    *relative = path;
    return 1;
}

int fat32_handles_path(const char *path)
{
    const char *relative;
    return path == 0 ? 0 : disk_path(path, &relative);
}

/* Resuelve una ruta FAT usando nombres cortos 8.3. */
static int resolve_path(const char *path, struct fat_node *node)
{
    const char *p;

    if (!disk_path(path, &p))
        return 0;

    node->cluster = vol.root_cluster;
    node->size = 0;
    node->attr = 0x10; /* directorio raiz */

    while (*p) {
        if (!(node->attr & 0x10))
            return 0;

        char component[256];
        uint32_t n = 0;

        while (*p == '/')
            p++;

        while (*p && *p != '/') {
            if (n >= sizeof(component) - 1)
                return 0;
            component[n++] = *p++;
        }
        component[n] = 0;

        if (!n)
            break;

        struct fat_node child;
        int rc = find_child(node->cluster, component, &child);

        if (rc != 1)
            return 0;

        *node = child;
    }

    return 1;
}

static int64_t fat_read(struct file *file, void *buffer, uint64_t count)
{
    struct fat_handle *h = file->priv;

    if (!h || (h->node.attr & 0x10))
        return -21; /* EISDIR */

    if (!count || file->off >= h->node.size)
        return 0;

    if (count > h->node.size - file->off)
        count = h->node.size - file->off;

    if (h->node.cluster < 2 || !valid_cluster(h->node.cluster))
        return -5; /* EIO */

    uint64_t cluster_bytes =
        (uint64_t)vol.sectors_per_cluster * vol.sector_size;
    uint64_t skip = file->off / cluster_bytes;
    uint64_t in_cluster = file->off % cluster_bytes;
    uint32_t cluster = h->node.cluster;

    for (uint64_t i = 0; i < skip; i++) {
        uint32_t next;
        if (next_cluster(cluster, &next) != 0)
            return -5;
        cluster = next;
    }

    uint8_t sector[FAT_SECTOR_MAX];
    uint8_t *dst = buffer;
    uint64_t done = 0;

    while (done < count) {
        uint32_t sector_in_cluster =
            (uint32_t)(in_cluster / vol.sector_size);
        uint32_t sector_offset =
            (uint32_t)(in_cluster % vol.sector_size);
        uint64_t lba;

        if (cluster_sector(cluster, sector_in_cluster, &lba) != 0 ||
            read_sector(lba, sector) != 0)
            break;

        uint64_t part = vol.sector_size - sector_offset;
        if (part > count - done)
            part = count - done;

        copy_bytes(dst + done, sector + sector_offset, (uint32_t)part);

        done += part;
        in_cluster += part;

        if (done < count && in_cluster >= cluster_bytes) {
            uint32_t next;
            if (next_cluster(cluster, &next) != 0)
                break;

            cluster = next;
            in_cluster = 0;
        }
    }

    if (!done && count)
        return -5;

    file->off += done;
    return (int64_t)done;
}

static int64_t fat_stat(struct file *file, void *st)
{
    struct fat_handle *h = file->priv;
    uint8_t *b = st;
    uint32_t mode = (h->node.attr & 0x10)
                  ? (0040000 | 0555)
                  : (0100000 | 0444);

    *(uint64_t *)(b + 16) = (h->node.attr & 0x10) ? 2 : 1;
    *(uint32_t *)(b + 24) = mode;
    *(int64_t *)(b + 48) = h->node.size;
    *(int64_t *)(b + 56) = vol.sector_size;
    *(int64_t *)(b + 64) = (h->node.size + 511u) / 512u;
    return 0;
}

static int64_t fat_lseek(struct file *file, int64_t offset, int whence)
{
    struct fat_handle *h = file->priv;
    int64_t base;

    if (whence == 0)
        base = 0;
    else if (whence == 1) {
        if (file->off > INT64_MAX)
            return -22;
        base = (int64_t)file->off;
    } else if (whence == 2)
        base = h->node.size;
    else
        return -22;

    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < -offset))
        return -22;

    int64_t next = base + offset;
    if (next < 0)
        return -22;

    file->off = (uint64_t)next;
    return next;
}

/* Lee una entrada de directorio por indice logico. */
static int directory_entry_at(uint32_t directory, uint64_t index,
                              uint8_t *entry)
{
    uint64_t entries_per_cluster =
        ((uint64_t)vol.sector_size * vol.sectors_per_cluster) / 32;

    if (!entries_per_cluster ||
        index / entries_per_cluster >= vol.cluster_count)
        return 0;

    uint64_t cluster_skip = index / entries_per_cluster;
    uint64_t local_index = index % entries_per_cluster;
    uint32_t cluster = directory;

    for (uint64_t i = 0; i < cluster_skip; i++) {
        uint32_t next;
        int rc = next_cluster(cluster, &next);
        if (rc == 1)
            return 0;
        if (rc != 0)
            return -1;
        cluster = next;
    }

    uint32_t entries_per_sector = vol.sector_size / 32;
    uint32_t sector_index = (uint32_t)(local_index / entries_per_sector);
    uint32_t entry_index = (uint32_t)(local_index % entries_per_sector);
    uint64_t lba;
    uint8_t sector[FAT_SECTOR_MAX];

    if (cluster_sector(cluster, sector_index, &lba) != 0 ||
        read_sector(lba, sector) != 0)
        return -1;

    copy_bytes(entry, sector + entry_index * 32, 32);
    if (entry[0] == 0)
        return 0;

    return 1;
}

static int64_t fat_getdents(struct file *file, void *buffer, uint64_t count)
{
    struct fat_handle *h = file->priv;

    if (!(h->node.attr & 0x10))
        return -20; /* ENOTDIR */

    if (!count)
        return 0;

    uint8_t entry[32];
    uint8_t *out = buffer;
    uint64_t used = 0;

    for (;;) {
        uint64_t cursor = file->off;
        int rc = directory_entry_at(h->node.cluster, cursor, entry);

        if (rc == 0)
            return (int64_t)used;
        if (rc < 0)
            return used ? (int64_t)used : -5;

        file->off = cursor + 1;

        if (hidden_entry(entry))
            continue;

        char name[FAT_NAME_MAX];
        short_name(entry, name);

        if (name_equal(name, ".") || name_equal(name, ".."))
            continue;

        uint64_t length = 0;
        while (name[length])
            length++;

        uint64_t reclen = (19 + length + 1 + 7) & ~7ULL;

        if (used + reclen > count) {
            file->off = cursor;
            return used ? (int64_t)used : -22;
        }

        uint8_t *r = out + used;
        zero_bytes(r, (uint32_t)reclen);
        *(uint64_t *)(r + 0) = ((uint64_t)h->node.cluster << 32) | cursor;
        *(int64_t *)(r + 8) = (int64_t)(cursor + 1);
        *(uint16_t *)(r + 16) = (uint16_t)reclen;
        r[18] = (entry[11] & 0x10) ? 4 : 8;

        for (uint64_t i = 0; i <= length; i++)
            r[19 + i] = (uint8_t)name[i];

        used += reclen;
    }
}


#ifdef FAT32_ENABLE_WRITES

#define FAT_EOC_VALUE 0x0FFFFFFFu
#define FAT_ERR_EIO   5
#define FAT_ERR_EINVAL 22
#define FAT_ERR_ENOSPC 28
#define FAT_ERR_EFBIG 27
#define FAT_ERR_ENOTSUP 95

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int write_sector(uint64_t relative, const uint8_t *buffer)
{
    if (!vol.mounted || relative >= vol.total_sectors ||
        vol.start_lba > UINT64_MAX - relative)
        return -1;

    return block_write(vol.device, vol.start_lba + relative,
                       1, buffer);
}

/* Actualiza la entrada FAT en las copias que corresponda. */

static int write_fsinfo_sector(uint32_t relative)
{
    uint8_t b[FAT_SECTOR_MAX];

    if (relative >= vol.reserved_sectors ||
        read_sector(relative, b) != 0 ||
        !fsinfo_signatures_valid(b))
        return -1;

    put_le32(b + 488, vol.free_count);
    put_le32(b + 492, vol.next_free);

    return write_sector(relative, b);
}

static int sync_fsinfo(void)
{
    if (!vol.fsinfo_valid)
        return 0;

    if (vol.fsinfo_sector == 0 ||
        vol.fsinfo_sector >= vol.reserved_sectors)
        return -1;

    if (write_fsinfo_sector(vol.fsinfo_sector) != 0)
        return -1;

    /*
     * La FSInfo de respaldo suele estar en
     * backup_boot_sector + fsinfo_sector.
     * Solo la modificamos si está en la región reservada
     * y conserva las firmas correctas.
     */
    if (vol.backup_boot_sector != 0 &&
        vol.backup_boot_sector != 0xFFFFu &&
        vol.backup_boot_sector < vol.reserved_sectors &&
        vol.fsinfo_sector <
            vol.reserved_sectors - vol.backup_boot_sector) {

        uint32_t backup_info =
            vol.backup_boot_sector + vol.fsinfo_sector;

        if (backup_info != vol.fsinfo_sector) {
            uint8_t b[FAT_SECTOR_MAX];

            if (read_sector(backup_info, b) == 0 &&
                fsinfo_signatures_valid(b)) {

                put_le32(b + 488, vol.free_count);
                put_le32(b + 492, vol.next_free);

                if (write_sector(backup_info, b) != 0)
                    return -1;
            }
        }
    }

    return 0;
}

/* Cuenta las entradas libres de la FAT activa, no las de FSInfo. */
static int recount_fat_free(uint32_t *free_out, uint32_t *hint_out)
{
    uint8_t buffer[FAT_SECTOR_MAX];

    if (!free_out || !hint_out || !vol.sector_size ||
        vol.sector_size > FAT_SECTOR_MAX)
        return -1;

    uint32_t sectors_per_read = FAT_SECTOR_MAX / vol.sector_size;
    uint32_t free_count = 0;
    uint32_t first_free = 0;
    uint32_t offset = 0;

    while (offset < vol.fat_size) {
        uint32_t count = vol.fat_size - offset;
        if (count > sectors_per_read)
            count = sectors_per_read;

        uint64_t relative = vol.fat_start + offset;

        if (relative >= vol.total_sectors ||
            count > vol.total_sectors - relative ||
            vol.start_lba > UINT64_MAX - relative)
            return -1;

        if (block_read(vol.device, vol.start_lba + relative,
                       count, buffer) != 0)
            return -1;

        uint32_t bytes = count * vol.sector_size;
        uint64_t byte_base = (uint64_t)offset * vol.sector_size;

        for (uint32_t pos = 0; pos + 4 <= bytes; pos += 4) {
            uint64_t index = (byte_base + pos) / 4;

            if (index < 2 ||
                index > (uint64_t)vol.cluster_count + 1)
                continue;

            if ((le32(buffer + pos) & 0x0FFFFFFFu) == 0) {
                free_count++;
                if (!first_free)
                    first_free = (uint32_t)index;
            }
        }

        offset += count;
    }

    *free_out = free_count;
    *hint_out = first_free ? first_free : 0xFFFFFFFFu;
    return 0;
}

static int set_fat_entry(uint32_t cluster, uint32_t value)
{
    if (!valid_cluster(cluster))
        return -1;

    uint64_t byte_offset = (uint64_t)cluster * 4;
    uint64_t sector_index = byte_offset / vol.sector_size;
    uint32_t byte_index = (uint32_t)(byte_offset % vol.sector_size);

    if (sector_index >= vol.fat_size)
        return -1;

    /* Leer el estado anterior desde la FAT activa. */
    uint8_t active_sector[FAT_SECTOR_MAX];

    if (read_sector(vol.fat_start + sector_index, active_sector) != 0)
        return -1;

    uint32_t old_value =
        le32(active_sector + byte_index) & 0x0FFFFFFFu;

    uint32_t first = vol.fat_mirroring ? 0 : vol.active_fat;
    uint32_t last = vol.fat_mirroring ? vol.fats : vol.active_fat + 1;
    uint8_t sector[FAT_SECTOR_MAX];

    for (uint32_t f = first; f < last; f++) {
        uint64_t lba = (uint64_t)vol.reserved_sectors
                     + (uint64_t)f * vol.fat_size + sector_index;

        if (read_sector(lba, sector) != 0)
            return -1;

        uint32_t old = le32(sector + byte_index);
        uint32_t updated = (old & 0xF0000000u)
                         | (value & 0x0FFFFFFFu);

        put_le32(sector + byte_index, updated);

        if (write_sector(lba, sector) != 0)
            return -1;
    }

    int was_free = old_value == 0;
    int now_free = (value & 0x0FFFFFFFu) == 0;

    if (was_free != now_free) {
        if (vol.free_count != 0xFFFFFFFFu) {
            if (was_free) {
                if (vol.free_count > 0)
                    vol.free_count--;
                else
                    vol.free_count = 0xFFFFFFFFu;
            } else if (vol.free_count < vol.cluster_count) {
                vol.free_count++;
            } else {
                vol.free_count = 0xFFFFFFFFu;
            }
        }

        /* El hint queda desconocido hasta el próximo recuento. */
        vol.next_free = 0xFFFFFFFFu;

        if (sync_fsinfo() != 0)
            console_puts("FAT32: ADVERTENCIA: fallo al sincronizar FSInfo\n");
    }

    return 0;
}

/* Reserva una entrada libre y pone a cero todos los sectores del cluster. */
static int allocate_cluster(uint32_t *allocated)
{
    uint8_t fat_sector[FAT_SECTOR_MAX];
    uint8_t zero[FAT_SECTOR_MAX];

    zero_bytes(zero, vol.sector_size);

    uint32_t entries_per_sector = vol.sector_size / 4;

    for (uint32_t s = 0; s < vol.fat_size; s++) {
        if (read_sector(vol.fat_start + s, fat_sector) != 0)
            return -1;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint64_t candidate64 = (uint64_t)s * entries_per_sector + i;

            if (candidate64 < 2 ||
                candidate64 > (uint64_t)vol.cluster_count + 1)
                continue;

            uint32_t candidate = (uint32_t)candidate64;

            if ((le32(fat_sector + i * 4) & 0x0FFFFFFFu) != 0)
                continue;

            if (set_fat_entry(candidate, FAT_EOC_VALUE) != 0)
                return -1;

            uint64_t start =
                vol.data_start +
                (uint64_t)(candidate - 2) * vol.sectors_per_cluster;

            for (uint32_t j = 0; j < vol.sectors_per_cluster; j++) {
                if (write_sector(start + j, zero) != 0) {
                    (void)set_fat_entry(candidate, 0);
                    return -1;
                }
            }

            *allocated = candidate;
            return 0;
        }
    }

    return -1;
}

/* Amplia la cadena hasta que contenga al menos "needed" clusters. */
static int ensure_clusters(struct fat_node *node, uint64_t needed)
{
    if (!needed)
        return 0;

    if (!node->cluster) {
        if (allocate_cluster(&node->cluster) != 0)
            return -1;
    }

    uint32_t current = node->cluster;

    for (uint64_t count = 1; count < needed; count++) {
        uint32_t next;
        int rc = next_cluster(current, &next);

        if (rc == 0) {
            current = next;
            continue;
        }

        if (rc < 0)
            return -1;

        uint32_t fresh;
        if (allocate_cluster(&fresh) != 0)
            return -1;

        if (set_fat_entry(current, fresh) != 0) {
            (void)set_fat_entry(fresh, 0);
            return -1;
        }

        current = fresh;
    }

    return 0;
}

/* Persiste cluster inicial y tamaño sin alterar el resto de la entrada. */
static int persist_node(struct fat_node *node)
{
    uint8_t sector[FAT_SECTOR_MAX];

    if (node->entry_offset > vol.sector_size - 32 ||
        (node->entry_offset & 31u) != 0 ||
        read_sector(node->entry_lba, sector) != 0)
        return -1;

    uint8_t *e = sector + node->entry_offset;

    if (e[0] == 0 || e[0] == 0xE5 || e[11] == 0x0F)
        return -1;

    put_le16(e + 20, (uint16_t)(node->cluster >> 16));
    put_le16(e + 26, (uint16_t)node->cluster);
    put_le32(e + 28, node->size);

    return write_sector(node->entry_lba, sector);
}

/* Libera una cadena completa, frenando ante ciclos o entradas inválidas. */
static int free_chain(uint32_t cluster)
{
    for (uint32_t step = 0; step < vol.cluster_count; step++) {
        uint32_t next;
        int rc = next_cluster(cluster, &next);

        if (rc < 0)
            return -1;

        if (set_fat_entry(cluster, 0) != 0)
            return -1;

        if (rc == 1)
            return 0;

        cluster = next;
    }

    return -1;
}

/*
 * Truncado seguro frente a una interrupción: primero se desconecta
 * la cadena del archivo; luego se liberan los clusters antiguos.
 * Un fallo puede dejar clusters perdidos, pero evita que el archivo
 * siga apuntando a clusters que ya se hayan marcado como libres.
 */
static int truncate_node(struct fat_node *node)
{
    uint32_t old_cluster = node->cluster;
    uint32_t old_size = node->size;

    node->cluster = 0;
    node->size = 0;

    if (persist_node(node) != 0) {
        node->cluster = old_cluster;
        node->size = old_size;
        return -1;
    }

    if (old_cluster && free_chain(old_cluster) != 0)
        return -1;

    return 0;
}

/* Acepta nombres 8.3 sencillos: NAME.TXT, TEST, KERNEL.ELF. */
static int encode_short_name(const char *name, uint8_t out[11])
{
    for (uint32_t i = 0; i < 11; i++)
        out[i] = ' ';

    uint32_t base = 0, ext = 0;
    int dot = 0;

    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '.') {
            if (dot || base == 0) return -1;
            dot = 1;
            continue;
        }

        if (c <= 32 || c >= 127 ||
            c == '"' || c == '*' || c == '+' || c == ',' ||
            c == '/' || c == ':' || c == ';' || c == '<' ||
            c == '=' || c == '>' || c == '?' || c == '[' ||
            c == '\\' || c == ']' || c == '|')
            return -1;

        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');

        if (!dot) {
            if (base >= 8) return -1;
            out[base++] = c;
        } else {
            if (ext >= 3) return -1;
            out[8 + ext++] = c;
        }
    }

    return base ? 0 : -1;
}

/* Busca una entrada disponible en la cadena del directorio raíz. */
static int find_free_root_slot(uint64_t *slot_lba, uint16_t *slot_offset)
{
    uint8_t sector[FAT_SECTOR_MAX];
    uint32_t cluster = vol.root_cluster;

    for (uint32_t step = 0; step < vol.cluster_count; step++) {
        for (uint32_t s = 0; s < vol.sectors_per_cluster; s++) {
            uint64_t lba;

            if (cluster_sector(cluster, s, &lba) != 0 ||
                read_sector(lba, sector) != 0)
                return -1;

            for (uint32_t off = 0; off < vol.sector_size; off += 32) {
                if (sector[off] == 0 || sector[off] == 0xE5) {
                    *slot_lba = lba;
                    *slot_offset = (uint16_t)off;
                    return 0;
                }
            }
        }

        uint32_t next;
        int rc = next_cluster(cluster, &next);

        if (rc == 1)
            return -1;
        if (rc != 0)
            return -1;

        cluster = next;
    }

    return -1;
}

/* Por ahora se crean archivos nuevos únicamente en la raíz de /disk. */
static int create_root_file(const char *path, struct fat_node *node)
{
    const char *relative;
    uint8_t shortname[11];
    uint8_t sector[FAT_SECTOR_MAX];
    uint8_t entry[32];

    if (!disk_path(path, &relative) || !*relative)
        return 21; /* EISDIR */

    for (const char *p = relative; *p; p++)
        if (*p == '/')
            return FAT_ERR_ENOTSUP;

    if (encode_short_name(relative, shortname) != 0)
        return FAT_ERR_EINVAL;

    struct fat_node existing;
    int found = find_child(vol.root_cluster, relative, &existing);

    if (found == 1)
        return 17; /* EEXIST */
    if (found < 0)
        return FAT_ERR_EIO;

    uint64_t slot_lba;
    uint16_t slot_offset;

    if (find_free_root_slot(&slot_lba, &slot_offset) != 0)
        return FAT_ERR_ENOSPC;

    if (read_sector(slot_lba, sector) != 0)
        return FAT_ERR_EIO;

    zero_bytes(entry, sizeof(entry));
    copy_bytes(entry, shortname, sizeof(shortname));

    entry[11] = 0x20; /* archivo normal */
    put_le16(entry + 16, 0x0021); /* fecha DOS mínima: 1980-01-01 */
    put_le16(entry + 18, 0x0021);
    put_le16(entry + 24, 0x0021);

    copy_bytes(sector + slot_offset, entry, sizeof(entry));

    if (write_sector(slot_lba, sector) != 0)
        return FAT_ERR_EIO;

    node->cluster = 0;
    node->size = 0;
    node->attr = 0x20;
    node->entry_lba = slot_lba;
    node->entry_offset = slot_offset;
    node->parent_cluster = vol.root_cluster;

    return 0;
}

#endif /* FAT32_ENABLE_WRITES */

static int64_t fat_write(struct file *file, const void *buffer,
                         uint64_t count)
{
#ifndef FAT32_ENABLE_WRITES
    (void)file;
    (void)buffer;
    (void)count;
    return -30; /* EROFS: escritura deshabilitada en la compilación normal */
#else
    struct fat_handle *h = file->priv;
    int64_t result = -FAT_ERR_EIO;
    uint64_t irq_flags = irq_save();

    if (!h || !(h->flags & FAT_O_ACCMODE)) {
        result = -9; /* EBADF */
        goto out;
    }

    if (!count) {
        result = 0;
        goto out;
    }

    if (!buffer || (h->node.attr & 0x10)) {
        result = -21; /* EISDIR */
        goto out;
    }

    if (h->flags & FAT_O_APPEND)
        file->off = h->node.size;

    if (file->off > h->node.size) {
        result = -FAT_ERR_EINVAL; /* No admitimos huecos dispersos todavía. */
        goto out;
    }

    if (file->off > UINT32_MAX ||
        count > UINT32_MAX - file->off) {
        result = -FAT_ERR_EFBIG;
        goto out;
    }

    uint64_t start = file->off;
    uint64_t end = start + count;
    uint64_t cluster_bytes =
        (uint64_t)vol.sectors_per_cluster * vol.sector_size;
    uint64_t required = (end + cluster_bytes - 1) / cluster_bytes;

    if (ensure_clusters(&h->node, required) != 0) {
        result = -FAT_ERR_ENOSPC;
        goto out;
    }

    uint32_t cluster = h->node.cluster;
    uint64_t skip = start / cluster_bytes;

    for (uint64_t i = 0; i < skip; i++) {
        uint32_t next;
        if (next_cluster(cluster, &next) != 0) {
            result = -FAT_ERR_EIO;
            goto out;
        }
        cluster = next;
    }

    const uint8_t *src = buffer;
    uint8_t sector[FAT_SECTOR_MAX];
    uint64_t done = 0;
    uint64_t position = start;

    while (done < count) {
        uint64_t in_cluster = position % cluster_bytes;
        uint32_t sector_in_cluster =
            (uint32_t)(in_cluster / vol.sector_size);
        uint32_t sector_offset =
            (uint32_t)(in_cluster % vol.sector_size);
        uint64_t lba;

        if (cluster_sector(cluster, sector_in_cluster, &lba) != 0 ||
            read_sector(lba, sector) != 0) {
            result = done ? (int64_t)done : -FAT_ERR_EIO;
            goto out;
        }

        uint64_t part = vol.sector_size - sector_offset;
        uint64_t left_cluster = cluster_bytes - in_cluster;

        if (part > left_cluster) part = left_cluster;
        if (part > count - done) part = count - done;

        copy_bytes(sector + sector_offset, src + done, (uint32_t)part);

        if (write_sector(lba, sector) != 0) {
            result = done ? (int64_t)done : -FAT_ERR_EIO;
            goto out;
        }

        done += part;
        position += part;

        if (done < count && position % cluster_bytes == 0) {
            uint32_t next;
            if (next_cluster(cluster, &next) != 0) {
                result = (int64_t)done;
                goto out;
            }
            cluster = next;
        }
    }

    if (end > h->node.size)
        h->node.size = (uint32_t)end;

    if (persist_node(&h->node) != 0) {
        result = -FAT_ERR_EIO;
        goto out;
    }

    file->off = end;
    result = (int64_t)done;

out:
    irq_restore(irq_flags);
    return result;
#endif
}

static int64_t fat_close(struct file *file)
{
    struct fat_handle *h = file->priv;
    if (h)
        h->used = 0;
    return 0;
}

static const struct file_ops fat_ops = {
    fat_read, fat_write, 0, fat_stat, fat_lseek, fat_getdents, fat_close
};

struct file *fat32_open_flags(const char *path, int flags, int *error)
{
    if (error)
        *error = 0;

    if (!vol.mounted) {
        if (error) *error = 19; /* ENODEV */
        return 0;
    }

    int write_access = (flags & FAT_O_ACCMODE) != 0;
    int create = (flags & FAT_O_CREAT) != 0;
    int exclusive = (flags & FAT_O_EXCL) != 0;
    int truncate = (flags & FAT_O_TRUNC) != 0;
    struct fat_node node;
    int found = resolve_path(path, &node);

#ifndef FAT32_ENABLE_WRITES
    if (write_access || create || truncate) {
        if (error) *error = 30; /* EROFS */
        return 0;
    }
#else
    if ((create || truncate) && !write_access) {
        if (error) *error = 22; /* EINVAL */
        return 0;
    }
#endif

    if (found) {
        if (create && exclusive) {
            if (error) *error = 17; /* EEXIST */
            return 0;
        }

        if ((node.attr & 0x10) && (write_access || truncate)) {
            if (error) *error = 21; /* EISDIR */
            return 0;
        }

#ifdef FAT32_ENABLE_WRITES
        if (truncate) {
            uint64_t irq_flags = irq_save();
            int rc = truncate_node(&node);
            irq_restore(irq_flags);

            if (rc != 0) {
                if (error) *error = 5; /* EIO */
                return 0;
            }
        }
#endif
    } else {
        if (!create) {
            if (error) *error = 2; /* ENOENT */
            return 0;
        }

#ifndef FAT32_ENABLE_WRITES
        if (error) *error = 30; /* EROFS */
        return 0;
#else
        if (!write_access) {
            if (error) *error = 22; /* EINVAL */
            return 0;
        }

        uint64_t irq_flags = irq_save();
        int rc = create_root_file(path, &node);
        irq_restore(irq_flags);

        if (rc != 0) {
            if (error) *error = rc;
            return 0;
        }
#endif
    }

    for (uint32_t i = 0; i < FAT_OPEN_MAX; i++) {
        struct fat_handle *h = &handles[i];

        if (h->used)
            continue;

        h->used = 1;
        h->flags = flags;
        h->node = node;
        h->file.ops = &fat_ops;
        h->file.refcount = 1;
        h->file.off = (flags & FAT_O_APPEND) ? node.size : 0;
        h->file.priv = h;
        return &h->file;
    }

    if (error) *error = 23; /* ENFILE */
    return 0;
}

struct file *fat32_open(const char *path)
{
    return fat32_open_flags(path, 0, 0);
}

static int try_mount(const struct partition_info *part,
                     uint32_t sector_size)
{
    uint8_t b[FAT_SECTOR_MAX];

    if (sector_size < 512 || sector_size > FAT_SECTOR_MAX ||
        part->sector_count < 2 ||
        block_read(part->device, part->first_lba, 1, b) != 0)
        return -1;

    if (b[510] != 0x55 || b[511] != 0xAA)
        return -1;

    uint32_t bps = le16(b + 11);
    uint32_t spc = b[13];
    uint32_t reserved = le16(b + 14);
    uint32_t fats = b[16];
    uint32_t root_entries = le16(b + 17);
    uint32_t total16 = le16(b + 19);
    uint32_t fatsz16 = le16(b + 22);
    uint32_t total32 = le32(b + 32);
    uint32_t fatsz32 = le32(b + 36);
    uint32_t ext_flags = le16(b + 40);
    uint32_t fs_version = le16(b + 42);
    uint32_t root = le32(b + 44) & 0x0FFFFFFFu;
    uint32_t fsinfo_sector = le16(b + 48);
    uint32_t backup_boot_sector = le16(b + 50);

    /* Solo FAT32; no interpretar FAT12/16 como si fuese FAT32. */
    if (bps != sector_size || root_entries != 0 ||
        fatsz16 != 0 || fatsz32 == 0 ||
        fs_version != 0 || !reserved ||
        !fats || fats > 4 ||
        !spc || (spc & (spc - 1)) ||
        (uint64_t)spc * bps > 65536)
        return -1;

    uint64_t total = total16 ? total16 : total32;

    if (!total || total > part->sector_count)
        return -1;

    uint32_t active_fat = 0;
    if (ext_flags & 0x80) {
        active_fat = ext_flags & 0x0F;
        if (active_fat >= fats)
            return -1;
    }

    uint64_t fats_total = (uint64_t)fats * fatsz32;
    uint64_t data_start = (uint64_t)reserved + fats_total;

    if (data_start >= total)
        return -1;

    uint64_t clusters = (total - data_start) / spc;

    if (!clusters || clusters > 0x0FFFFFEFu ||
        root < 2 || root > clusters + 1)
        return -1;

    /* Comprobar que la FAT tenga una entrada para cada cluster. */
    uint64_t fat_bytes = (uint64_t)fatsz32 * bps;
    if (fat_bytes / 4 < clusters + 2)
        return -1;

    vol.device = part->device;
    vol.sector_size = bps;
    vol.sectors_per_cluster = spc;
    vol.reserved_sectors = reserved;
    vol.fats = fats;
    vol.fat_size = fatsz32;
    vol.active_fat = active_fat;
    vol.fat_mirroring = !(ext_flags & 0x80);
    vol.root_cluster = root;
    vol.cluster_count = (uint32_t)clusters;
    vol.start_lba = part->first_lba;
    vol.partition_sectors = part->sector_count;
    vol.total_sectors = total;
    vol.fat_start = (uint64_t)reserved + (uint64_t)active_fat * fatsz32;
    vol.data_start = data_start;
    vol.fsinfo_sector = fsinfo_sector;
    vol.backup_boot_sector = backup_boot_sector;
    vol.free_count = 0xFFFFFFFFu;
    vol.next_free = 0xFFFFFFFFu;
    vol.fsinfo_valid = 0;
    vol.mounted = 1;

    if (fsinfo_sector > 0 && fsinfo_sector < reserved &&
        fsinfo_sector != 0xFFFFu) {
        uint8_t info[FAT_SECTOR_MAX];
        if (read_sector(fsinfo_sector, info) == 0 &&
            fsinfo_signatures_valid(info)) {
            uint32_t fc = le32(info + 488);
            uint32_t nf = le32(info + 492);
            vol.fsinfo_valid = 1;
            if (fc <= clusters)
                vol.free_count = fc;
            if (nf >= 2 && nf <= clusters + 1)
                vol.next_free = nf;
        }
    }

    console_puts("FAT32: volumen valido encontrado\n");
    console_puts("FAT32: bytes/sector=");
    console_dec(bps);
    console_puts(", sectores/cluster=");
    console_dec(spc);
    console_puts(", cluster raiz=");
    console_dec(root);
    console_putc('\n');

    return 0;
}

void fat32_init(void)
{
    vol.mounted = 0;

    console_puts("\nFAT32: buscando volumenes...\n");

    uint32_t n = partition_count();

    for (uint32_t i = 0; i < n; i++) {
        struct partition_info part;
        uint64_t sectors;
        uint32_t sector_size;

        if (partition_get(i, &part) != 0 ||
            block_get_info(part.device, &sectors, &sector_size) != 0)
            continue;

        if (part.first_lba >= sectors ||
            part.sector_count > sectors - part.first_lba)
            continue;

        if (try_mount(&part, sector_size) == 0) {
#ifdef FAT32_ENABLE_WRITES
            uint32_t actual_free = 0;
            uint32_t actual_hint = 0;
            uint32_t recorded_free = vol.free_count;

            if (recount_fat_free(&actual_free, &actual_hint) == 0) {
                if (vol.fsinfo_valid &&
                    recorded_free != 0xFFFFFFFFu &&
                    recorded_free != actual_free) {
                    console_puts("FAT32: FSInfo desactualizado; guardado=");
                    console_dec(recorded_free);
                    console_puts(", real=");
                    console_dec(actual_free);
                    console_putc('\n');
                }

                vol.free_count = actual_free;
                vol.next_free = actual_hint;

                if (vol.fsinfo_valid) {
                    if (sync_fsinfo() == 0)
                        console_puts("FAT32: FSInfo sincronizado con la FAT\n");
                    else
                        console_puts("FAT32: ADVERTENCIA: fallo al guardar FSInfo\n");
                } else {
                    console_puts("FAT32: FSInfo no valido; no se modifica\n");
                }
            } else {
                vol.free_count = 0xFFFFFFFFu;
                vol.next_free = 0xFFFFFFFFu;
                console_puts("FAT32: ADVERTENCIA: no se pudo recalcular espacio libre\n");
            }
#endif
            #ifdef FAT32_ENABLE_WRITES
            console_puts("FAT32: montado en /disk, ESCRITURA EXPERIMENTAL\n");
#else
            #ifdef FAT32_ENABLE_WRITES
            console_puts("FAT32: montado en /disk, ESCRITURA EXPERIMENTAL\n");
#else
            console_puts("FAT32: montado en /disk, solo lectura\n");
#endif
#endif
            return;
        }
    }

    console_puts("FAT32: no se encontro volumen compatible\n");
}
