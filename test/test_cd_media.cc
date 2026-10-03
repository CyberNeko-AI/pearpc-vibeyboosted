// Exercise the real controller packet path and media request queue, without
// a guest OS. CPU DMA memory and PIC interrupts are the only test doubles.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include "../src/io/ide/ide.cc"

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
void ppc_fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); exit(1);
}
void ppc_set_singlestep_v(bool, const char *, int, const char *, ...) { ppc_fatal("unexpected debugger entry\n"); }
void pic_raise_interrupt(int) {}
void pic_cancel_interrupt(int) {}
uint32 ppc_cpu_get_pc(int) { return 0; }
static byte memory[65536];
byte *ppc_dma_get_ptr(uint32 addr, uint32 size)
{
    return addr <= sizeof(memory) && size <= sizeof(memory) - addr ? memory + addr : nullptr;
}
bool ppc_dma_read(void *dest, uint32 src, uint32 size)
{
    byte *p = ppc_dma_get_ptr(src, size); if (!p) return false; memcpy(dest, p, size); return true;
}
bool ppc_dma_write(uint32 dest, const void *src, uint32 size)
{
    byte *p = ppc_dma_get_ptr(dest, size); if (!p) return false; memcpy(p, src, size); return true;
}
static void image(const std::string &path, unsigned sectors, byte value)
{
    FILE *f = fopen(path.c_str(), "wb"); CHECK(f != nullptr);
    byte b[2048]; memset(b, value, sizeof(b));
    for (unsigned i = 0; i < sectors; i++) CHECK(fwrite(b, 1, sizeof(b), f) == sizeof(b));
    fclose(f);
}
static void wr(IDE_Controller &ctl, uint32 reg, uint32 val, uint size = 1)
{
    CHECK(ctl.writeDeviceIO(IDE_PCI_REG_0_CMD, reg, val, size));
}
static uint32 rd(IDE_Controller &ctl, uint32 reg, uint size = 1)
{
    uint32 val = 0; CHECK(ctl.readDeviceIO(IDE_PCI_REG_0_CMD, reg, val, size)); return val;
}
static void packet(IDE_Controller &ctl, byte command, byte arg4 = 0, unsigned blocks = 0)
{
    byte cdb[12] = {}; cdb[0] = command; cdb[4] = arg4;
    cdb[7] = blocks >> 8; cdb[8] = blocks;
    wr(ctl, IDE_ADDRESS_CYL_LSB, 0xfe);
    wr(ctl, IDE_ADDRESS_CYL_MSB, 0xff);
    wr(ctl, IDE_ADDRESS_COMMAND, IDE_COMMAND_PACKET);
    for (unsigned i = 0; i < 12; i += 2) wr(ctl, IDE_ADDRESS_DATA, cdb[i] | (cdb[i+1] << 8), 2);
}
static void sense(IDE_Controller &ctl, byte key, byte asc, byte ascq = 0)
{
    packet(ctl, IDE_ATAPI_COMMAND_REQ_SENSE, 18);
    byte data[18];
    for (unsigned i = 0; i < sizeof(data); i += 2) {
        uint32 word = rd(ctl, IDE_ADDRESS_DATA, 2); data[i] = word; data[i+1] = word >> 8;
    }
    CHECK(data[2] == key && data[12] == asc && data[13] == ascq);
}
static void result(bool expected)
{
    bool ok; std::string message; CHECK(ide_take_media_result(ok, message)); CHECK(ok == expected);
}
int main()
{
    char temp[] = "/tmp/pearpc-cd-media.XXXXXX"; CHECK(mkdtemp(temp) != nullptr);
    std::string a = std::string(temp) + "/one.iso", b = std::string(temp) + "/two.iso";
    image(a, 4, 0x11); image(b, 7, 0x22);
    {
        CDROMDeviceFile dvd("configured DVD", true);
        CHECK(dvd.changeDataSource(a.c_str())); CHECK(dvd.isDVD());
        dvd.eject(); CHECK(dvd.changeDataSource(b.c_str())); CHECK(dvd.isDVD());
    }
    CDROMDeviceFile disc("test optical"); CHECK(disc.changeDataSource(a.c_str())); disc.setReady(true);
    memset(&gIDEState, 0, sizeof(gIDEState));
    // Deliberately use IDE slave: optical index 0 must still select this drive.
    gIDEState.config[0].installed = true; gIDEState.config[0].protocol = IDE_ATA;
    gIDEState.config[1].installed = true; gIDEState.config[1].protocol = IDE_ATAPI;
    gIDEState.config[1].device = &disc; gIDEState.config[1].bps = 2048;
    gIDEState.state[1].status = IDE_STATUS_RDY;
    IDE_Controller ctl;
    wr(ctl, IDE_ADDRESS_DRV_HEAD, 0x10);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_ERR));

    // Host callback queues while a real multi-sector PIO read is in progress.
    packet(ctl, IDE_ATAPI_COMMAND_READ10, 0, 2);
    std::thread ui([&]() { CHECK(ide_request_cd_change(0, b)); }); ui.join();
    CHECK(!ide_request_cd_change(0, a));
    for (unsigned i = 0; i < 2048; i++) CHECK(rd(ctl, IDE_ADDRESS_DATA, 2) == 0x1111);
    CHECK(disc.getCapacity() == 4); // request cannot take effect within PIO data
    rd(ctl, IDE_ADDRESS_STATUS); result(true); CHECK(disc.getCapacity() == 7);
    // INQUIRY must not consume the change notification.
    packet(ctl, IDE_ATAPI_COMMAND_INQUIRY, 36);
    for (int i = 0; i < 18; i++) rd(ctl, IDE_ADDRESS_DATA, 2);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); CHECK(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_ERR);
    sense(ctl, 6, 0x28); sense(ctl, 0, 0);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_ERR));
    packet(ctl, IDE_ATAPI_COMMAND_READ10, 0, 1);
    for (int i = 0; i < 1024; i++) CHECK(rd(ctl, IDE_ADDRESS_DATA, 2) == 0x2222);

    // Failed open and malformed image preserve medium, capacity and readiness.
    CHECK(ide_request_cd_change(0, std::string(temp) + "/missing.iso")); rd(ctl, IDE_ADDRESS_STATUS);
    result(false); CHECK(disc.isReady() && disc.getCapacity() == 7);
    std::string bad = std::string(temp) + "/bad.iso";
    FILE *f = fopen(bad.c_str(), "wb"); CHECK(f); fputs("invalid", f); fclose(f);
    CHECK(ide_request_cd_change(0, bad)); rd(ctl, IDE_ADDRESS_STATUS); result(false);
    CHECK(disc.isReady() && disc.getCapacity() == 7);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); CHECK(!(rd(ctl, IDE_ADDRESS_STATUS) & IDE_STATUS_ERR));

    packet(ctl, IDE_ATAPI_COMMAND_TOGGLE_LOCK, 1); CHECK(disc.isLocked());
    CHECK(ide_request_cd_change(0, a)); rd(ctl, IDE_ADDRESS_STATUS); result(false);
    packet(ctl, IDE_ATAPI_COMMAND_START_STOP, 2); sense(ctl, 5, 0x53, 2);
    CHECK(disc.isReady() && disc.getCapacity() == 7);
    packet(ctl, IDE_ATAPI_COMMAND_TOGGLE_LOCK, 0); CHECK(!disc.isLocked());
    packet(ctl, IDE_ATAPI_COMMAND_START_STOP, 2); CHECK(!disc.isReady() && disc.getCapacity() == 0);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); sense(ctl, 6, 0x28);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); sense(ctl, 2, 0x3a);
    byte empty[2048]; CHECK(disc.read(empty, sizeof(empty)) == 0); CHECK(disc.readBlock(empty) < 0);

    CHECK(ide_request_cd_change(0, a)); rd(ctl, IDE_ADDRESS_STATUS); result(true);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); sense(ctl, 6, 0x28);
    // A start/load request must not eject an inserted disc.
    packet(ctl, IDE_ATAPI_COMMAND_START_STOP, 3); CHECK(disc.isReady());
    CHECK(ide_request_cd_change(0, "")); rd(ctl, IDE_ADDRESS_STATUS); result(true); CHECK(!disc.isReady());
    CHECK(ide_request_cd_change(1, a)); rd(ctl, IDE_ADDRESS_STATUS); result(false);

    // The DMA engine holds the device across calls, even with DRQ clear.
    disc.acquire();
    CHECK(ide_request_cd_change(0, b)); rd(ctl, IDE_ADDRESS_STATUS);
    bool ok; std::string message;
    CHECK(!ide_take_media_result(ok, message)); CHECK(!disc.isReady());
    disc.release(); rd(ctl, IDE_ADDRESS_STATUS); result(true); CHECK(disc.getCapacity() == 7);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); sense(ctl, 6, 0x28);
    // A failed backend open must preserve the current position too.
    CHECK(disc.seek(3)); CHECK(!disc.changeDataSource(bad.c_str()));
    byte sample[2048]; CHECK(disc.read(sample, sizeof(sample)) == sizeof(sample)); CHECK(sample[0] == 0x22);

    // Real DMA handshake: PACKET is accepted before bus-master START. The
    // packet receive lock is released, and DRQ is clear, but media must wait.
    uint32 prd[] = {ppc_word_to_LE(0x1000u), ppc_word_to_LE(0x80001000u)};
    memcpy(memory + 0x100, prd, sizeof(prd));
    CHECK(ctl.writeDeviceIO(IDE_PCI_REG_BMDMA, 4, 0x100, 4));
    wr(ctl, IDE_ADDRESS_FEATURE, 1);
    packet(ctl, IDE_ATAPI_COMMAND_READ10, 0, 2);
    CHECK(gIDEState.state[1].mode == IDE_TRANSFER_MODE_DMA);
    CHECK(!disc.isAcquired());
    CHECK(ide_request_cd_change(0, a)); rd(ctl, IDE_ADDRESS_STATUS);
    CHECK(!ide_take_media_result(ok, message)); CHECK(disc.getCapacity() == 7);
    CHECK(ctl.writeDeviceIO(IDE_PCI_REG_BMDMA, 0, BM_IDE_CR_WRITE | BM_IDE_CR_START, 1));
    for (unsigned i = 0; i < 4096; i++) CHECK(memory[0x1000 + i] == 0x22);
    rd(ctl, IDE_ADDRESS_STATUS); result(true); CHECK(disc.getCapacity() == 4);
    CHECK(ctl.writeDeviceIO(IDE_PCI_REG_BMDMA, 0, 0, 1));
    wr(ctl, IDE_ADDRESS_FEATURE, 0);
    packet(ctl, IDE_ATAPI_COMMAND_TEST_READY); sense(ctl, 6, 0x28);

    // Two optical drives: the first optical is now master, second is slave.
    CDROMDeviceFile master("second test optical"); CHECK(master.changeDataSource(a.c_str())); master.setReady(true);
    gIDEState.config[0].protocol = IDE_ATAPI; gIDEState.config[0].device = &master;
    gIDEState.state[0].status = IDE_STATUS_RDY;
    CHECK(ide_request_cd_change(0, b)); CHECK(ide_request_cd_change(1, a));
    rd(ctl, IDE_ADDRESS_STATUS); result(true); result(true);
    CHECK(master.getCapacity() == 7 && disc.getCapacity() == 4);
    CHECK(master.consumeMediaChange()); CHECK(disc.consumeMediaChange());
    CHECK(ide_request_cd_change(0, "")); rd(ctl, IDE_ADDRESS_STATUS); result(true);
    CHECK(!master.isReady() && disc.isReady());
    CHECK(master.consumeMediaChange()); CHECK(!disc.consumeMediaChange());
    CHECK(!ide_request_cd_change(2, a));

    unlink(a.c_str()); unlink(b.c_str()); unlink(bad.c_str()); rmdir(temp);
    printf("PASS: %u CD media/controller checks\n", checks);
}
