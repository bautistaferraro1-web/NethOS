#include <stdint.h>

#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "mem.h"
#include "console.h"
#include "cpu.h"

#define NVME_REG_CAP       0x0000
#define NVME_REG_VS        0x0008
#define NVME_REG_CC        0x0014
#define NVME_REG_CSTS      0x001C
#define NVME_REG_AQA       0x0024
#define NVME_REG_ASQ       0x0028
#define NVME_REG_ACQ       0x0030
#define NVME_REG_DOORBELL  0x1000

#define NVME_OP_CREATE_SQ  0x01
#define NVME_OP_READ       0x02
#define NVME_OP_CREATE_CQ  0x05
#define NVME_OP_IDENTIFY   0x06

#define NVME_ADMIN_QID     0
#define NVME_IO_QID        1
#define NVME_PAGE_SIZE     4096ULL
#define NVME_MAX_MMIO      0x4000ULL
#define NVME_POLL_LIMIT    20000000ULL

struct nvme_command {
    uint32_t dw[16];
};

struct nvme_completion {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
};

_Static_assert(sizeof(struct nvme_command) == 64,
               "NVMe command must be 64 bytes");
_Static_assert(sizeof(struct nvme_completion) == 16,
               "NVMe completion must be 16 bytes");

struct nvme_queue {
    volatile struct nvme_command *sq;
    volatile struct nvme_completion *cq;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    uint16_t depth;
    uint16_t tail;
    uint16_t head;
    uint16_t next_cid;
    uint8_t phase;
};

static volatile uint8_t *nvme_regs;
static uint32_t nvme_stride;

/* Estado persistente del dispositivo una vez inicializado. */
static struct nvme_queue nvme_adminq;
static struct nvme_queue nvme_ioq;
static uint64_t nvme_sector_count;
static uint64_t nvme_data_phys;
static uint32_t nvme_sector_size;
static int nvme_ready;

static uint32_t reg_read32(uint32_t off)
{
    return *(volatile uint32_t *)(nvme_regs + off);
}

static void reg_write32(uint32_t off, uint32_t value)
{
    *(volatile uint32_t *)(nvme_regs + off) = value;
}

static uint64_t reg_read64(uint32_t off)
{
    return *(volatile uint64_t *)(nvme_regs + off);
}

static void reg_write64(uint32_t off, uint64_t value)
{
    *(volatile uint64_t *)(nvme_regs + off) = value;
}

static void nvme_fence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

static void zero_page(uint64_t phys)
{
    volatile uint64_t *p = (volatile uint64_t *)P2V(phys);
    for (unsigned i = 0; i < 512; i++)
        p[i] = 0;
}

/* MMIO usa una página dedicada marcada como uncached. */
static int map_mmio_page(uint64_t phys)
{
    uint64_t page = phys & ~(NVME_PAGE_SIZE - 1);
    return vmm_map(P2V(page), page, VMM_WRITE | VMM_PCD | VMM_PWT);
}

static int wait_ready(uint32_t ready)
{
    for (uint64_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint32_t status = reg_read32(NVME_REG_CSTS);

        if (status & (1u << 1)) {
            console_puts("[nvme] CSTS indica fallo fatal: ");
            console_hex(status);
            console_putc('\n');
            return -1;
        }

        if ((status & 1u) == ready)
            return 0;
    }

    console_puts("[nvme] timeout esperando CSTS.RDY: ");
    console_hex(reg_read32(NVME_REG_CSTS));
    console_putc('\n');
    return -1;
}

static void queue_init(struct nvme_queue *q,
                       uint64_t sq_phys, uint64_t cq_phys,
                       uint16_t depth, uint16_t qid)
{
    q->sq = (volatile struct nvme_command *)P2V(sq_phys);
    q->cq = (volatile struct nvme_completion *)P2V(cq_phys);
    q->sq_db = (volatile uint32_t *)(nvme_regs + NVME_REG_DOORBELL
                                    + (2u * qid) * nvme_stride);
    q->cq_db = (volatile uint32_t *)(nvme_regs + NVME_REG_DOORBELL
                                    + (2u * qid + 1u) * nvme_stride);
    q->depth = depth;
    q->tail = 0;
    q->head = 0;
    q->next_cid = 1;
    q->phase = 1;
}

static void command_init(struct nvme_queue *q,
                         struct nvme_command *cmd,
                         uint8_t opcode, uint32_t nsid)
{
    for (unsigned i = 0; i < 16; i++)
        cmd->dw[i] = 0;

    cmd->dw[0] = (uint32_t)opcode
               | ((uint32_t)q->next_cid++ << 16);
    cmd->dw[1] = nsid;
}

/* PRP1 está en DW4-DW5; DW6-DW7 corresponden a PRP2. */
static void command_prp1(struct nvme_command *cmd, uint64_t phys)
{
    /* DW4-DW5 = MPTR; DW6-DW7 = PRP1. */
    cmd->dw[6] = (uint32_t)phys;
    cmd->dw[7] = (uint32_t)(phys >> 32);
}

/* Una orden pendiente por cola; sondeo sin interrupciones. */
static int submit(struct nvme_queue *q, const struct nvme_command *cmd)
{
    volatile uint32_t *dst = (volatile uint32_t *)&q->sq[q->tail];

    for (unsigned i = 0; i < 16; i++)
        dst[i] = cmd->dw[i];

    q->tail++;
    if (q->tail >= q->depth)
        q->tail = 0;

    nvme_fence();
    *q->sq_db = q->tail;
    nvme_fence();

    for (uint64_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint16_t status = q->cq[q->head].status;

        if ((status & 1u) != q->phase)
            continue;

        nvme_fence();

        q->head++;
        if (q->head >= q->depth) {
            q->head = 0;
            q->phase ^= 1u;
        }

        *q->cq_db = q->head;
        nvme_fence();

        if (status & 0xFFFEu) {
            console_puts("[nvme] comando fallo; status=");
            console_hex(status);
            console_putc('\n');
            return -1;
        }

        return 0;
    }

    console_puts("[nvme] timeout esperando completion\n");
    return -1;
}

static void print_hex_byte(uint8_t v)
{
    static const char h[] = "0123456789ABCDEF";
    console_putc(h[v >> 4]);
    console_putc(h[v & 0x0F]);
}

void nvme_init(void)
{
    struct pci_nvme_device dev;

    console_puts("\nNVMe: inicializando controlador...\n");

    if (pci_find_nvme(&dev) != 0) {
        console_puts("NVMe: no se encontro controlador PCI\n");
        return;
    }

    uint64_t bar = dev.bar0;

    /* Por ahora soportamos BAR MMIO dentro del mapa físico de 4 GiB. */
    if ((bar & (NVME_PAGE_SIZE - 1)) ||
        bar < NVME_PAGE_SIZE ||
        bar + NVME_MAX_MMIO > 0x100000000ULL) {
        console_puts("NVMe: BAR fuera del rango soportado: ");
        console_hex(bar);
        console_putc('\n');
        return;
    }

    if (pci_enable_nvme(&dev) != 0) {
        console_puts("NVMe: no se pudo habilitar PCI\n");
        return;
    }

    if (map_mmio_page(bar) != 0) {
        console_puts("NVMe: fallo mapeando BAR0\n");
        return;
    }

    nvme_regs = (volatile uint8_t *)P2V(bar);

    uint64_t cap = reg_read64(NVME_REG_CAP);
    uint32_t vs = reg_read32(NVME_REG_VS);
    uint32_t entries = (uint32_t)(cap & 0xFFFFu) + 1u;
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0x0Fu);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0x0Fu);

    console_puts("NVMe: CAP=");
    console_hex(cap);
    console_puts(" VS=");
    console_hex(vs);
    console_putc('\n');

    if (mpsmin != 0) {
        console_puts("NVMe: no admite páginas de 4 KiB\n");
        return;
    }

    if (entries < 2) {
        console_puts("NVMe: profundidad de cola insuficiente\n");
        return;
    }

    nvme_stride = 4u << dstrd;

    uint64_t mmio_bytes = NVME_REG_DOORBELL
                        + 3ULL * nvme_stride
                        + sizeof(uint32_t);

    if (mmio_bytes > NVME_MAX_MMIO) {
        console_puts("NVMe: stride de doorbells fuera de rango\n");
        return;
    }

    uint64_t mmio_pages =
        (mmio_bytes + NVME_PAGE_SIZE - 1) / NVME_PAGE_SIZE;

    for (uint64_t i = 1; i < mmio_pages; i++) {
        if (map_mmio_page(bar + i * NVME_PAGE_SIZE) != 0) {
            console_puts("NVMe: fallo mapeando doorbells\n");
            return;
        }
    }

    uint16_t depth = entries < 16 ? (uint16_t)entries : 16;

    uint64_t dma[7];
    unsigned allocated = 0;

    for (; allocated < 7; allocated++) {
        dma[allocated] = pmm_alloc_page();
        if (!dma[allocated]) {
            console_puts("NVMe: no hay páginas para DMA\n");
            for (unsigned j = 0; j < allocated; j++)
                pmm_free_page(dma[j]);
            return;
        }
        zero_page(dma[allocated]);
    }

    uint64_t asq_phys = dma[0];
    uint64_t acq_phys = dma[1];
    uint64_t identify_ctrl_phys = dma[2];
    uint64_t identify_ns_phys = dma[3];
    uint64_t iosq_phys = dma[4];
    uint64_t iocq_phys = dma[5];
    uint64_t data_phys = dma[6];
    nvme_data_phys = data_phys;

    /* Deshabilitar antes de programar las colas administrativas. */
    reg_write32(NVME_REG_CC, 0);
    if (wait_ready(0) != 0)
        return;

    struct nvme_queue *admin = &nvme_adminq;
    queue_init(admin, asq_phys, acq_phys, depth, NVME_ADMIN_QID);

    reg_write32(NVME_REG_AQA,
        ((uint32_t)(depth - 1) << 16) | (uint32_t)(depth - 1));
    reg_write64(NVME_REG_ASQ, asq_phys);
    reg_write64(NVME_REG_ACQ, acq_phys);

    /* CC.EN=1, IOSQES=6 (64 bytes), IOCQES=4 (16 bytes). */
    uint32_t cc = (6u << 16) | (4u << 20) | 1u;
    nvme_fence();
    reg_write32(NVME_REG_CC, cc);

    if (wait_ready(1) != 0) {
        reg_write32(NVME_REG_CC, 0);
        return;
    }

    console_puts("NVMe: controlador listo\n");

    struct nvme_command cmd;

    /* Identify Controller, CNS=1. */
    command_init(admin, &cmd, NVME_OP_IDENTIFY, 0);
    command_prp1(&cmd, identify_ctrl_phys);
    cmd.dw[10] = 1;

    if (submit(admin, &cmd) != 0) {
        console_puts("NVMe: Identify Controller fallo\n");
        return;
    }

    uint8_t *ctrl = (uint8_t *)P2V(identify_ctrl_phys);
    char model[41];

    for (unsigned i = 0; i < 40; i++)
        model[i] = (char)ctrl[24 + i];
    model[40] = '\0';

    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';

    console_puts("NVMe modelo: ");
    console_puts(model);
    console_putc('\n');

    /* Identify Namespace 1, CNS=0. */
    command_init(admin, &cmd, NVME_OP_IDENTIFY, 1);
    command_prp1(&cmd, identify_ns_phys);
    cmd.dw[10] = 0;

    if (submit(admin, &cmd) != 0) {
        console_puts("NVMe: Identify Namespace fallo\n");
        return;
    }

    uint8_t *ns = (uint8_t *)P2V(identify_ns_phys);
    uint64_t sectors = *(uint64_t *)(void *)ns;
    uint8_t nlbaf = ns[25];
    uint8_t flbas = ns[26] & 0x0Fu;

    if (!sectors || flbas > nlbaf) {
        console_puts("NVMe: namespace sin formato LBA valido\n");
        return;
    }

    uint8_t lbads = ns[128 + (unsigned)flbas * 4 + 2];
    if (lbads < 9 || lbads > 16) {
        console_puts("NVMe: tamaño LBA no soportado\n");
        return;
    }

    uint32_t sector_size = 1u << lbads;

    console_puts("NVMe: namespace 1, sectores=");
    console_dec(sectors);
    console_puts(", bytes/sector=");
    console_dec(sector_size);
    console_putc('\n');

    if (sector_size > NVME_PAGE_SIZE) {
        console_puts("NVMe: lectura omitida; sector mayor que 4 KiB\n");
        return;
    }

    nvme_sector_count = sectors;
    nvme_sector_size = sector_size;

    /* Crear CQ y SQ de I/O físicamente contiguas. */
    queue_init(&nvme_ioq, iosq_phys, iocq_phys, depth, NVME_IO_QID);

    command_init(admin, &cmd, NVME_OP_CREATE_CQ, 0);
    command_prp1(&cmd, iocq_phys);
    cmd.dw[10] = ((uint32_t)(depth - 1) << 16) | NVME_IO_QID;
    cmd.dw[11] = 1u;  /* PC=1, interrupciones deshabilitadas */

    if (submit(admin, &cmd) != 0) {
        console_puts("NVMe: Create I/O CQ fallo\n");
        return;
    }

    command_init(admin, &cmd, NVME_OP_CREATE_SQ, 0);
    command_prp1(&cmd, iosq_phys);
    cmd.dw[10] = ((uint32_t)(depth - 1) << 16) | NVME_IO_QID;
    cmd.dw[11] = (1u << 16) | 1u; /* CQID=1, PC=1 */

    if (submit(admin, &cmd) != 0) {
        console_puts("NVMe: Create I/O SQ fallo\n");
        return;
    }

    /* La cola de I/O está lista para usar la API pública. */
    nvme_ready = 1;

    /* Read de un bloque lógico en SLBA=0. NLB=0 significa un bloque. */
    command_init(&nvme_ioq, &cmd, NVME_OP_READ, 1);
    command_prp1(&cmd, data_phys);
    cmd.dw[10] = 0;
    cmd.dw[11] = 0;
    cmd.dw[12] = 0;

    if (submit(&nvme_ioq, &cmd) != 0) {
        console_puts("NVMe: lectura de LBA 0 fallo\n");
        return;
    }

    uint8_t *data = (uint8_t *)P2V(data_phys);
    static const uint8_t signature[16] = {
        'N','E','T','H','R','A','-',
        'N','V','M','E','-','L','B','A','0'
    };

    int matches = 1;
    for (unsigned i = 0; i < sizeof(signature); i++) {
        if (data[i] != signature[i]) {
            matches = 0;
            break;
        }
    }

    if (matches) {
        console_puts("NVMe: LBA 0 leido; firma de prueba OK\n");
    } else {
        console_puts("NVMe: LBA 0 leido; primeros bytes: ");
        for (unsigned i = 0; i < 16; i++) {
            print_hex_byte(data[i]);
            console_putc(i == 15 ? '\n' : ' ');
        }
    }

#ifdef NVME_TEST_WRITES
    nvme_selftest_write();
#endif
}


static int nvme_rw_blocks(uint64_t lba, uint32_t count,
                          void *buffer, int is_write)
{
    uint64_t irq_flags = irq_save();
    int result = -1;

    if (!nvme_ready || !buffer || !count || !nvme_sector_size)
        goto out;

    if (lba >= nvme_sector_count ||
        (uint64_t)count > nvme_sector_count - lba)
        goto out;

    if ((uint64_t)count * nvme_sector_size > NVME_PAGE_SIZE)
        goto out;

    uint64_t bytes = (uint64_t)count * nvme_sector_size;
    volatile uint8_t *dma =
        (volatile uint8_t *)P2V(nvme_data_phys);
    uint8_t *buf = (uint8_t *)buffer;

    if (is_write) {
        for (uint64_t i = 0; i < bytes; i++)
            dma[i] = buf[i];
        nvme_fence();
    }

    struct nvme_command cmd;
    command_init(&nvme_ioq, &cmd,
                 is_write ? 0x01u : NVME_OP_READ, 1);
    command_prp1(&cmd, nvme_data_phys);
    cmd.dw[10] = (uint32_t)lba;
    cmd.dw[11] = (uint32_t)(lba >> 32);
    cmd.dw[12] = count - 1u;

    if (submit(&nvme_ioq, &cmd) != 0)
        goto out;

    if (!is_write) {
        nvme_fence();
        for (uint64_t i = 0; i < bytes; i++)
            buf[i] = dma[i];
    }

    result = 0;

out:
    irq_restore(irq_flags);
    return result;
}

int nvme_read_blocks(uint64_t lba, uint32_t count, void *buffer)
{
    return nvme_rw_blocks(lba, count, buffer, 0);
}

int nvme_write_blocks(uint64_t lba, uint32_t count, const void *buffer)
{
    return nvme_rw_blocks(lba, count, (void *)buffer, 1);
}

#ifdef NVME_TEST_WRITES
void nvme_selftest_write(void)
{
    static uint8_t expected[NVME_PAGE_SIZE];
    static uint8_t actual[NVME_PAGE_SIZE];

    if (!nvme_ready || nvme_sector_count <= 8 ||
        !nvme_sector_size || nvme_sector_size > sizeof(expected)) {
        console_puts("NVMe TEST WRITE: no se puede probar\n");
        return;
    }

    /*
     * Esta prueba solo debe compilarse al usar el disco virtual desechable
     * de QEMU. LBA 8 está después de la firma de prueba de LBA 0.
     */
    for (uint32_t i = 0; i < nvme_sector_size; i++)
        expected[i] = (uint8_t)(i * 37u + 11u);

    console_puts("NVMe TEST WRITE: probando LBA 8 en disco virtual...\n");

    if (nvme_write_blocks(8, 1, expected) != 0) {
        console_puts("NVMe TEST WRITE: escritura FALLIDA\n");
        return;
    }

    for (uint32_t i = 0; i < nvme_sector_size; i++)
        actual[i] = 0;

    if (nvme_read_blocks(8, 1, actual) != 0) {
        console_puts("NVMe TEST WRITE: lectura posterior FALLIDA\n");
        return;
    }

    for (uint32_t i = 0; i < nvme_sector_size; i++) {
        if (actual[i] != expected[i]) {
            console_puts("NVMe TEST WRITE: comparacion FALLIDA\n");
            return;
        }
    }

    console_puts("NVMe TEST WRITE: write/read LBA 8 OK\n");
}
#endif
