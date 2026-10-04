// Exercise raw image validation, guest IDENTIFY capacity and tail-sector I/O.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/io/ide/ide.cc"

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
void ppc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
void ppc_set_singlestep_v(bool, const char *, int, const char *, ...) { abort(); }
void pic_raise_interrupt(int) {}
void pic_cancel_interrupt(int) {}
uint32 ppc_cpu_get_pc(int) { return 0; }
byte *ppc_dma_get_ptr(uint32, uint32) { return nullptr; }
bool ppc_dma_read(void *, uint32, uint32) { return false; }
bool ppc_dma_write(uint32, const void *, uint32) { return false; }

static void wr(IDE_Controller &ctl, uint32 reg, uint32 value, uint size = 1)
{
    CHECK(ctl.writeDeviceIO(IDE_PCI_REG_0_CMD, reg, value, size));
}
static uint32 rd(IDE_Controller &ctl, uint32 reg, uint size = 1)
{
    uint32 value = 0;
    CHECK(ctl.readDeviceIO(IDE_PCI_REG_0_CMD, reg, value, size));
    return value;
}
static void selectLBA(IDE_Controller &ctl, uint32 lba)
{
    wr(ctl, IDE_ADDRESS_DRV_HEAD, 0xe0 | (lba >> 24));
    wr(ctl, IDE_ADDRESS_SEC_CNT, 1);
    wr(ctl, IDE_ADDRESS_SEC_NO, lba & 0xff);
    wr(ctl, IDE_ADDRESS_CYL_LSB, (lba >> 8) & 0xff);
    wr(ctl, IDE_ADDRESS_CYL_MSB, (lba >> 16) & 0xff);
}
static void createImage(const char *path, uint64 bytes)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, bytes) == 0);
    CHECK(close(fd) == 0);
}
static void testImage(const char *path, uint64 bytes)
{
    createImage(path, bytes);
    {
        ATADeviceFile disk("capacity test", path);
        if (disk.getError()) fprintf(stderr, "Open %llu bytes: %s\n", bytes, disk.getError());
        CHECK(disk.getError() == nullptr);
        CHECK(disk.getBlockCount() == bytes / 512);
        File *raw = disk.promGetRawFile();
        CHECK(raw->getSize() == bytes); // Must not overflow at 4 GiB / 8 GiB.
        delete raw;
        if (bytes >= 516096) {
            CHECK(disk.mHeads == 16 && disk.mSpt == 63);
            CHECK(disk.mCyl == bytes / 516096);
        } else {
            CHECK(disk.mHeads == 1 && disk.mSpt == 1 && disk.mCyl == bytes / 512);
        }
        memset(&gIDEState, 0, sizeof gIDEState);
        IDEConfig &cfg = gIDEState.config[0];
        cfg.installed = true;
        cfg.protocol = IDE_ATA;
        cfg.device = &disk;
        cfg.bps = 512;
        cfg.hd.cyl = disk.mCyl;
        cfg.hd.heads = disk.mHeads;
        cfg.hd.spt = disk.mSpt;
        gIDEState.state[0].status = IDE_STATUS_RDY;
        IDE_Controller ctl;
        wr(ctl, IDE_ADDRESS_DRV_HEAD, 0xe0);
        wr(ctl, IDE_ADDRESS_COMMAND, IDE_COMMAND_IDENT);
        CHECK(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_DRQ);
        uint16 id[256];
        for (unsigned i = 0; i < 256; ++i) id[i] = rd(ctl, IDE_ADDRESS_DATA, 2);
        CHECK((id[49] & (1 << 9)) != 0); // LBA advertised.
        CHECK(((uint32)id[60] | ((uint32)id[61] << 16)) == bytes / 512);
        uint32 chsSectors = disk.mCyl * disk.mHeads * disk.mSpt;
        CHECK(((uint32)id[57] | ((uint32)id[58] << 16)) == chsSectors);
        CHECK(chsSectors <= disk.getBlockCount());
        CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DRQ)));

        // Guest PIO write/read at the last sector, including non-CHS tails.
        uint32 last = disk.getBlockCount() - 1;
        selectLBA(ctl, last);
        wr(ctl, IDE_ADDRESS_COMMAND, IDE_COMMAND_WRITE_SECTOR);
        CHECK(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_DRQ);
        for (unsigned i = 0; i < 256; ++i) wr(ctl, IDE_ADDRESS_DATA, i ^ 0xa55a, 2);
        CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_ERR));
        selectLBA(ctl, last);
        wr(ctl, IDE_ADDRESS_COMMAND, IDE_COMMAND_READ_SECTOR);
        CHECK(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_DRQ);
        for (unsigned i = 0; i < 256; ++i) CHECK(rd(ctl, IDE_ADDRESS_DATA, 2) == (i ^ 0xa55a));
        CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & (IDE_STATUS_ERR | IDE_STATUS_DRQ)));
        disk.flush();
    }
    struct stat st;
    CHECK(stat(path, &st) == 0 && (uint64)st.st_size == bytes);
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    byte tail[512];
    CHECK(pread(fd, tail, sizeof tail, bytes - sizeof tail) == sizeof tail);
    for (unsigned i = 0; i < 256; ++i) CHECK((tail[2*i] | (tail[2*i+1] << 8)) == (i ^ 0xa55a));
    CHECK(close(fd) == 0);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    const uint64 sizes[] = {8ULL << 30, 512, 1007 * 512, 516096, 516096 + 512, 12 * 516096};
    for (uint64 size : sizes) testImage(argv[1], size);
    for (uint64 size : {0ULL, 1ULL, 511ULL, 513ULL, 65535ULL * 516096 + 512}) {
        createImage(argv[1], size);
        ATADeviceFile disk("invalid test", argv[1]);
        CHECK(disk.getError() != nullptr);
    }
    CHECK(unlink(argv[1]) == 0);
    printf("PASS: %u ATA capacity / IDENTIFY / tail-sector PIO checks\n", checks);
}
