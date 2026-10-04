// Drive the real CUDA registers through Linux via-cuda's receive protocol.
// No guest, UI, worker thread or disk images are needed.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../src/io/cuda/cuda.cc"

static bool irqPending;
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

SystemKeyboard *gKeyboard = nullptr;
void ppc_cpu_stop() { abort(); }
void pic_raise_interrupt(int irq) { CHECK(irq == IO_PIC_IRQ_CUDA); irqPending = true; }
void pic_cancel_interrupt(int) { irqPending = false; }
void ppc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
static uint32 rd(uint32 reg)
{
    uint32 value = 0;
    cuda_read(IO_CUDA_PA_START + reg, value, 1);
    return value;
}
static void wr(uint32 reg, uint32 value)
{
    cuda_write(IO_CUDA_PA_START + reg, value, 1);
}
static void takeIRQ(const char *phase)
{
    if (!irqPending) {
        fprintf(stderr, "Missing IRQ in %s: state=%d left=%d B=%02x IFR=%02x\n",
                phase, gCUDA.state, gCUDA.left, gCUDA.rB, gCUDA.rIFR);
    }
    CHECK(irqPending);
    irqPending = false;
    uint32 flags = rd(IFR) & 0x7f;
    wr(IFR, flags);
    CHECK(flags & SR_INT);
}
static unsigned status()
{
    return (~rd(B) & (TIP | TREQ)) | (rd(ACR) & SR_OUT);
}
static std::vector<byte> receivePacket()
{
    // Linux idle: acknowledge the request, then start receiving.
    takeIRQ("idle/request");
    CHECK(status() == TREQ);
    rd(SR);
    wr(B, rd(B) & ~TIP);
    std::vector<byte> packet;
    for (unsigned i = 0; i < 16; ++i) {
        takeIRQ("reading/byte");
        unsigned st = status();
        CHECK(st == TIP || st == (TIP | TREQ));
        packet.push_back(rd(SR));
        if (st == TIP) {
            wr(B, rd(B) | TACK | TIP);
            // Linux read_done: delivery of the completed packet happens only
            // in this additional interrupt, even when no next key is queued.
            takeIRQ("read_done/completion");
            CHECK(status() == 0);
            rd(SR);
            CHECK(!irqPending);
            return packet;
        }
        wr(B, rd(B) ^ TACK);
    }
    CHECK(false);
    return packet;
}
static void sendRequest(const std::vector<byte> &request)
{
    CHECK(request.size() >= 2);
    CHECK(gCUDA.state == cuda_idle && gCUDA.left == 0);
    CHECK(!irqPending);
    wr(ACR, rd(ACR) | SR_OUT);
    wr(SR, request[0]);
    wr(B, rd(B) & ~TIP);
    for (size_t i = 1; i < request.size(); ++i) {
        takeIRQ("sending/byte");
        CHECK(status() == (TIP | SR_OUT));
        wr(SR, request[i]);
        wr(B, rd(B) ^ TACK);
    }
    takeIRQ("sending/last-byte");
    CHECK(status() == (TIP | SR_OUT));
    wr(ACR, rd(ACR) & ~SR_OUT);
    rd(SR);
    wr(B, rd(B) | TACK | TIP);
}
int main()
{
    CHECK(sys_create_mutex(&gCUDAMutex) == 0);
    CHECK(sys_create_semaphore(&gCUDA.idle_sem) == 0);
    sys_semaphore idle = gCUDA.idle_sem;
    for (unsigned initialAck : {0u, (unsigned)TACK}) {
        gCUDA = {};
        gCUDA.idle_sem = idle;
        gCUDA.state = cuda_idle;
        gCUDA.rB = TIP | TREQ | initialAck;
        gCUDA.rACR = SR_EXT;
        gCUDA.keybaddr = 2;
        gCUDA.keybhandler = 1;
        gCUDA.rT1LL = gCUDA.rT1LH = 0xff;
        irqPending = false;
        for (unsigned key : {0x02u, 0x2eu, 0x0eu, 0x01u, 0x05u}) { // dmesg
            for (bool pressed : {true, false}) {
                SystemEvent ev = {};
                ev.type = sysevKey;
                ev.key.keycode = key;
                ev.key.pressed = pressed;
                CHECK(doProcessCudaEvent(ev));
                const std::vector<byte> expected = {0, 0x40, 0x2c,
                    (byte)(key | (pressed ? 0 : 0x80)), 0xff};
                CHECK(receivePacket() == expected);
                CHECK(gCUDA.state == cuda_idle && gCUDA.left == 0);
            }
        }
        // Cover command/reply turnaround and both even and odd packet sizes.
        sendRequest({CUDA_PACKET, CUDA_AUTOPOLL, 1});
        CHECK(receivePacket() == std::vector<byte>({CUDA_PACKET, 1}));
        CHECK(gCUDA.autopoll);
        sendRequest({ADB_PACKET, 0x2f}); // keyboard register 3
        CHECK(receivePacket() == std::vector<byte>({ADB_PACKET, 0, 1, 2}));
        sendRequest({CUDA_PACKET, CUDA_GET_TIME});
        std::vector<byte> clock = receivePacket();
        CHECK(clock.size() == 7 && clock[0] == CUDA_PACKET);
        SystemEvent mouse = {};
        mouse.type = sysevMouse;
        mouse.mouse.relx = 1;
        mouse.mouse.rely = -1;
        CHECK(doProcessCudaEvent(mouse));
        CHECK(receivePacket() == std::vector<byte>({ADB_PACKET, 0x40, 0x3c, 0xff, 0x81}));
    }
    sys_destroy_semaphore(idle);
    sys_destroy_mutex(gCUDAMutex);
    printf("PASS: %u checks; Linux receives every dmesg press/release without a subsequent event; command/reply and mouse transfers complete\n", checks);
}
