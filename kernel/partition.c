#include <stdint.h>

#include "partition.h"
#include "block.h"
#include "console.h"

#define PARTITION_MAX       32u
#define SECTOR_BUFFER_SIZE  4096u
#define GPT_HEADER_MIN      92u
#define GPT_ENTRY_MIN       128u
#define GPT_ENTRY_MAX       1024u
#define GPT_ENTRIES_MAX     8192u
#define GPT_ARRAY_MAX       (1024u * 1024u)

static struct partition_info partitions[PARTITION_MAX];
static uint32_t partitions_used;

/* Lecturas little-endian sin depender de alineacion. */
static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p)
         | ((uint64_t)get_le32(p + 4) << 32);
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int all_zero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != 0)
            return 0;
    return 1;
}

static void bytes_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

/* CRC-32/IEEE, el utilizado por las estructuras GPT. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data,
                             uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];

        for (unsigned b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }

    return crc;
}

static uint32_t gpt_header_crc(const uint8_t *header, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    for (uint32_t i = 0; i < length; i++) {
        /* El campo header_crc32 debe tratarse como cero. */
        uint8_t byte = (i >= 16 && i < 20) ? 0 : header[i];
        crc = crc32_update(crc, &byte, 1);
    }

    return crc ^ 0xFFFFFFFFu;
}

static void make_gpt_name(char *dst, const uint8_t *entry,
                          uint32_t entry_size)
{
    uint32_t out = 0;

    for (uint32_t off = 56; off + 1 < entry_size && out < 36; off += 2) {
        uint16_t ch = get_le16(entry + off);

        if (ch == 0)
            break;

        /* Nombres ASCII legibles; los demas caracteres se sustituyen. */
        dst[out++] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
    }

    if (out == 0) {
        static const char fallback[] = "GPT partition";

        for (uint32_t i = 0; i < sizeof(fallback); i++)
            dst[i] = fallback[i];
    } else {
        dst[out] = 0;
    }
}

static int save_partition(uint32_t device, uint32_t scheme,
                          uint32_t index, uint8_t mbr_type,
                          const uint8_t *type_guid,
                          const uint8_t *unique_guid,
                          uint64_t first_lba, uint64_t sector_count,
                          uint64_t attributes, const char *name)
{
    if (partitions_used >= PARTITION_MAX || sector_count == 0)
        return -1;

    struct partition_info *p = &partitions[partitions_used];

    p->device = device;
    p->scheme = scheme;
    p->index = index;
    p->mbr_type = mbr_type;
    p->first_lba = first_lba;
    p->sector_count = sector_count;
    p->attributes = attributes;

    for (uint32_t i = 0; i < 16; i++) {
        p->type_guid[i] = type_guid ? type_guid[i] : 0;
        p->unique_guid[i] = unique_guid ? unique_guid[i] : 0;
    }

    uint32_t i = 0;
    if (name) {
        while (name[i] && i < sizeof(p->name) - 1) {
            p->name[i] = name[i];
            i++;
        }
    }
    p->name[i] = 0;

    partitions_used++;
    return 0;
}

/*
 * Retornos:
 *   1: GPT primaria validada correctamente.
 *   0: no hay cabecera GPT primaria reconocible.
 *  -1: hay firma GPT, pero la estructura o su CRC no son validos.
 */
static int scan_gpt(uint32_t device, uint64_t disk_sectors,
                    uint32_t sector_size)
{
    static const uint8_t signature[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };

    uint8_t sector[SECTOR_BUFFER_SIZE];

    if (block_read(device, 1, 1, sector) != 0)
        return 0;

    if (!bytes_equal(sector, signature, sizeof(signature)))
        return 0;

    console_puts("GPT: cabecera primaria encontrada\n");

    uint32_t header_size = get_le32(sector + 12);
    uint32_t header_crc = get_le32(sector + 16);

    uint64_t current_lba = get_le64(sector + 24);
    uint64_t backup_lba = get_le64(sector + 32);
    uint64_t first_usable = get_le64(sector + 40);
    uint64_t last_usable = get_le64(sector + 48);
    uint64_t entries_lba = get_le64(sector + 72);

    uint32_t entry_count = get_le32(sector + 80);
    uint32_t entry_size = get_le32(sector + 84);
    uint32_t entries_crc = get_le32(sector + 88);

    if (header_size < GPT_HEADER_MIN ||
        header_size > sector_size ||
        current_lba != 1 ||
        backup_lba >= disk_sectors ||
        backup_lba == current_lba ||
        first_usable > last_usable ||
        last_usable >= disk_sectors) {
        console_puts("GPT: campos de cabecera invalidos\n");
        return -1;
    }

    if (gpt_header_crc(sector, header_size) != header_crc) {
        console_puts("GPT: CRC de cabecera incorrecto\n");
        return -1;
    }

    if (entry_count == 0 ||
        entry_count > GPT_ENTRIES_MAX ||
        entry_size < GPT_ENTRY_MIN ||
        entry_size > GPT_ENTRY_MAX ||
        (entry_size & 7u) != 0) {
        console_puts("GPT: tabla de entradas no soportada\n");
        return -1;
    }

    uint64_t array_bytes = (uint64_t)entry_count * entry_size;

    if (array_bytes > GPT_ARRAY_MAX)
        return -1;

    uint64_t array_sectors =
        (array_bytes + sector_size - 1) / sector_size;

    if (entries_lba < 2 ||
        entries_lba >= disk_sectors ||
        array_sectors > disk_sectors - entries_lba) {
        console_puts("GPT: tabla fuera de los limites del disco\n");
        return -1;
    }

    uint64_t array_end = entries_lba + array_sectors;

    /* La tabla no debe invadir el intervalo util declarado por GPT. */
    if (entries_lba <= last_usable && array_end > first_usable) {
        console_puts("GPT: la tabla invade el area de particiones\n");
        return -1;
    }

    uint32_t before = partitions_used;
    uint32_t entry_fill = 0;
    uint32_t entry_index = 0;
    uint8_t entry[GPT_ENTRY_MAX];

    uint64_t bytes_left = array_bytes;
    uint64_t sector_offset = 0;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t max_chunk = SECTOR_BUFFER_SIZE / sector_size;

    while (bytes_left) {
        uint64_t sectors_needed =
            (bytes_left + sector_size - 1) / sector_size;

        uint32_t chunk_count = sectors_needed < max_chunk
                             ? (uint32_t)sectors_needed : max_chunk;

        if (block_read(device, entries_lba + sector_offset,
                       chunk_count, sector) != 0) {
            partitions_used = before;
            console_puts("GPT: error leyendo la tabla de particiones\n");
            return -1;
        }

        uint64_t chunk_capacity = (uint64_t)chunk_count * sector_size;
        uint32_t chunk_bytes = bytes_left < chunk_capacity
                             ? (uint32_t)bytes_left
                             : (uint32_t)chunk_capacity;

        crc = crc32_update(crc, sector, chunk_bytes);

        uint32_t pos = 0;

        while (pos < chunk_bytes) {
            uint32_t need = entry_size - entry_fill;
            uint32_t available = chunk_bytes - pos;
            uint32_t take = need < available ? need : available;

            bytes_copy(entry + entry_fill, sector + pos, take);

            entry_fill += take;
            pos += take;

            if (entry_fill == entry_size) {
                if (!all_zero(entry, 16)) {
                    uint64_t first = get_le64(entry + 32);
                    uint64_t last = get_le64(entry + 40);
                    uint64_t attrs = get_le64(entry + 48);

                    if (first <= last &&
                        first >= first_usable &&
                        last <= last_usable &&
                        last < disk_sectors) {
                        char name[37];

                        make_gpt_name(name, entry, entry_size);

                        save_partition(device, PARTITION_SCHEME_GPT,
                                       entry_index + 1, 0,
                                       entry, entry + 16,
                                       first, last - first + 1,
                                       attrs, name);
                    } else {
                        console_puts("GPT: entrada invalida omitida\n");
                    }
                }

                entry_index++;
                entry_fill = 0;
            }
        }

        bytes_left -= chunk_bytes;
        sector_offset += chunk_count;
    }

    crc ^= 0xFFFFFFFFu;

    if (entry_fill != 0 || entry_index != entry_count ||
        crc != entries_crc) {
        partitions_used = before;
        console_puts("GPT: CRC o longitud de tabla incorrectos\n");
        return -1;
    }

    console_puts("GPT: cabecera y CRC de tabla validos\n");
    return 1;
}

static int is_extended_mbr_type(uint8_t type)
{
    return type == 0x05 || type == 0x0F || type == 0x85;
}

/* Reconoce entradas MBR primarias; aun no recorre cadenas EBR extendidas. */
static int scan_mbr(uint32_t device, uint64_t disk_sectors,
                    uint32_t sector_size)
{
    uint8_t sector[SECTOR_BUFFER_SIZE];

    if (sector_size < 512 ||
        block_read(device, 0, 1, sector) != 0)
        return 0;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;

    uint32_t before = partitions_used;
    int protective = 0;

    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t *e = sector + 446 + i * 16;
        uint8_t type = e[4];
        uint64_t first = get_le32(e + 8);
        uint64_t count = get_le32(e + 12);

        if (type == 0 || count == 0)
            continue;

        if (type == 0xEE) {
            protective = 1;
            continue;
        }

        if (is_extended_mbr_type(type)) {
            console_puts("MBR: particion extendida detectada; EBR pendiente\n");
            continue;
        }

        if (first == 0 ||
            first >= disk_sectors ||
            count > disk_sectors - first) {
            console_puts("MBR: entrada fuera de los limites, omitida\n");
            continue;
        }

        save_partition(device, PARTITION_SCHEME_MBR, i + 1,
                       type, 0, 0, first, count, 0, "MBR primary");
    }

    if (protective && partitions_used == before)
        console_puts("MBR: firma protectora GPT presente\n");

    return 1;
}

static void print_partitions(void)
{
    console_puts("\nParticiones detectadas: ");
    console_dec(partitions_used);
    console_putc('\n');

    for (uint32_t i = 0; i < partitions_used; i++) {
        const struct partition_info *p = &partitions[i];

        console_puts("  dispositivo ");
        console_dec(p->device);
        console_puts(", ");

        console_puts(p->scheme == PARTITION_SCHEME_GPT ? "GPT #" : "MBR #");
        console_dec(p->index);

        console_puts(", nombre=");
        console_puts(p->name);
        console_puts(", inicio LBA=");
        console_dec(p->first_lba);
        console_puts(", sectores=");
        console_dec(p->sector_count);

        if (p->scheme == PARTITION_SCHEME_MBR) {
            console_puts(", tipo=");
            console_hex(p->mbr_type);
        }

        console_putc('\n');
    }
}

void partition_init(void)
{
    partitions_used = 0;

    uint32_t devices = block_device_count();

    console_puts("\nPART: buscando tablas de particiones...\n");

    if (devices == 0) {
        console_puts("PART: no hay dispositivos de bloques\n");
        return;
    }

    for (uint32_t d = 0; d < devices; d++) {
        uint64_t disk_sectors = 0;
        uint32_t sector_size = 0;

        if (block_get_info(d, &disk_sectors, &sector_size) != 0)
            continue;

        if (sector_size < 512 ||
            sector_size > SECTOR_BUFFER_SIZE ||
            (SECTOR_BUFFER_SIZE % sector_size) != 0) {
            console_puts("PART: tamano de sector no soportado\n");
            continue;
        }

        console_puts("PART: dispositivo ");
        console_dec(d);
        console_puts(", sectores=");
        console_dec(disk_sectors);
        console_putc('\n');

        uint32_t before = partitions_used;
        int gpt = scan_gpt(d, disk_sectors, sector_size);

        if (gpt != 1) {
            if (gpt < 0)
                console_puts("PART: GPT primaria invalida; probando MBR\n");

            scan_mbr(d, disk_sectors, sector_size);
        }

        if (partitions_used == before)
            console_puts("PART: no se encontraron particiones utilizables\n");
    }

    print_partitions();
}

uint32_t partition_count(void)
{
    return partitions_used;
}

int partition_get(uint32_t index, struct partition_info *out)
{
    if (!out || index >= partitions_used)
        return -1;

    *out = partitions[index];
    return 0;
}
