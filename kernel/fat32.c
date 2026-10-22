#include <stdint.h>

#include "fat32.h"
#include "block.h"
#include "partition.h"
#include "console.h"

#define FAT_SECTOR_MAX 4096u
#define FAT_OPEN_MAX   32u
#define FAT_EOC_MIN    0x0FFFFFF8u
#define FAT_BAD        0x0FFFFFF7u
#define FAT_NAME_MAX   13u

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
};

struct fat_node {
    uint32_t cluster;
    uint32_t size;
    uint8_t attr;
};

struct fat_handle {
    struct file file;
    int used;
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
    return vol.mounted && path && disk_path(path, &relative);
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

static int64_t fat_close(struct file *file)
{
    struct fat_handle *h = file->priv;
    if (h)
        h->used = 0;
    return 0;
}

static const struct file_ops fat_ops = {
    fat_read, 0, 0, fat_stat, fat_lseek, fat_getdents, fat_close
};

struct file *fat32_open(const char *path)
{
    struct fat_node node;

    if (!vol.mounted || !resolve_path(path, &node))
        return 0;

    if (!(node.attr & 0x10) &&
        (node.cluster < 2 || !valid_cluster(node.cluster)) &&
        node.size != 0)
        return 0;

    for (uint32_t i = 0; i < FAT_OPEN_MAX; i++) {
        struct fat_handle *h = &handles[i];

        if (h->used)
            continue;

        h->used = 1;
        h->node = node;
        h->file.ops = &fat_ops;
        h->file.refcount = 1;
        h->file.off = 0;
        h->file.priv = h;
        return &h->file;
    }

    return 0;
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
    vol.root_cluster = root;
    vol.cluster_count = (uint32_t)clusters;
    vol.start_lba = part->first_lba;
    vol.partition_sectors = part->sector_count;
    vol.total_sectors = total;
    vol.fat_start = (uint64_t)reserved + (uint64_t)active_fat * fatsz32;
    vol.data_start = data_start;
    vol.mounted = 1;

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
            console_puts("FAT32: montado en /disk, solo lectura\n");
            return;
        }
    }

    console_puts("FAT32: no se encontro volumen compatible\n");
}
