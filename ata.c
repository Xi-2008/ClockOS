#include "ata.h"

// 直接在这里写端口操作，不依赖 kernel.c 里的函数
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

int g_ata_slave = 0;
static int g_ata_ready = 0;

// 延时 400ns，等待总线稳定
static void ata_delay_400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(0x3F6);
}

// 软复位：当硬盘彻底卡死时，强制让它重新启动
static void ata_soft_reset(void) {
    outb(0x3F6, 0x04); // 发送 SRST (Software Reset) 信号
    for (volatile int i = 0; i < 100000; i++); // 给硬件一点反应时间
    outb(0x3F6, 0x00); // 清除 SRST 信号
    for (volatile int i = 0; i < 1000000; i++); // 等待复位完成
}

// 探测设备
static int ata_identify(int slave) {
    outb(0x1F6, 0xA0 | (slave ? 0x10 : 0x00));
    ata_delay_400ns();
    if (inb(0x1F7) == 0xFF) return 0;

    outb(0x1F6, 0xA0 | (slave ? 0x10 : 0x00));
    outb(0x1F2, 0);
    outb(0x1F3, 0);
    outb(0x1F4, 0);
    outb(0x1F5, 0);
    outb(0x1F7, 0xEC); // IDENTIFY
    ata_delay_400ns();
    if (inb(0x1F7) == 0) return 0;

    for (int i = 0; i < 200000; i++) {
        uint8_t st = inb(0x1F7);
        if (st == 0) return 0;
        if (!(st & 0x80)) {
            uint8_t mid = inb(0x1F4);
            uint8_t hi  = inb(0x1F5);
            if (mid != 0 || hi != 0) return 0; 
            if (st & 0x01) return 0;           

            // 必须把 256 个 word 的数据读走，清空 DRQ 状态！
            for (int w = 0; w < 256; w++) {
                (void)inw(0x1F0);
            }
            // 等待 DRQ 清零
            for (int w = 0; w < 100000; w++) {
                if (!(inb(0x1F7) & 0x08)) break;
            }
            return 1;
        }
    }
    return 0;
}

// 硬件初始化
int ata_init(void) {
    g_ata_ready = 0;
    g_ata_slave = 0;
    if (inb(0x1F7) == 0xFF) return 0; // 没有 IDE 控制器

    // 优先探测主盘 (Master)，如果没有，探测从盘 (Slave)
    if (ata_identify(0)) { g_ata_slave = 0; g_ata_ready = 1; return 1; }
    if (ata_identify(1)) { g_ata_slave = 1; g_ata_ready = 1; return 1; }
    return 0;
}

// 等待 BSY 清零（带超时）
static int ata_wait_bsy(void) {
    uint32_t timeout = 1000000; // 约几十毫秒的超时
    while (timeout--) {
        uint8_t status = inb(0x1F7);
        if (status == 0xFF || status == 0x00) return ATA_ERR_NOTRDY;
        if (!(status & 0x80)) return ATA_OK;
    }
    return ATA_ERR_TIMEOUT;
}

// 等待 DRQ 置位（带超时和 ERR 检查）
static int ata_wait_drq(void) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        uint8_t status = inb(0x1F7);
        if (status == 0xFF || status == 0x00) return ATA_ERR_NOTRDY;
        if (status & 0x01) return ATA_ERR_IO; // ERR 置位，设备报错
        if (!(status & 0x80) && (status & 0x08)) return ATA_OK; // BSY=0 且 DRQ=1，完美就绪
    }
    return ATA_ERR_TIMEOUT;
}

// 严格的等待就绪，拒绝任何错位
static int ata_wait_ready_strict(void) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        uint8_t status = inb(0x1F7);
        if (status == 0xFF || status == 0x00) return ATA_ERR_NOTRDY;
        if (status & 0x01) return ATA_ERR_IO; // ERR 置位
        // 必须满足：不忙(BSY=0) 且 就绪(DRDY=1)
        if (!(status & 0x80) && (status & 0x40)) return ATA_OK; 
    }
    return ATA_ERR_TIMEOUT;
}

// 极其严格的读扇区
int ata_read_sector(uint32_t lba, uint8_t* buffer) {
    if (!buffer || !g_ata_ready) return ATA_ERR_INVAL;

    for (int retry = 0; retry < 3; retry++) {
        if (ata_wait_ready_strict() != ATA_OK) continue;

        outb(0x1F6, 0xE0 | (g_ata_slave << 4) | ((lba >> 24) & 0x0F));
        outb(0x1F2, 1);
        outb(0x1F3, (uint8_t)lba);
        outb(0x1F4, (uint8_t)(lba >> 8));
        outb(0x1F5, (uint8_t)(lba >> 16));
        outb(0x1F7, 0x20); // READ

        uint32_t timeout = 1000000;
        int ready = 0;
        while (timeout--) {
            uint8_t status = inb(0x1F7);
            if (status & 0x01) break; // 报错
            if (!(status & 0x80) && (status & 0x08)) { ready = 1; break; } // DRQ 就绪
        }
        if (!ready) continue;

        uint16_t *ptr = (uint16_t*)buffer;
        for (int i = 0; i < 256; i++) {
            *ptr++ = inw(0x1F0);
        }
        return ATA_OK;
    }
    return ATA_ERR_TIMEOUT;
}

// 极其严格的写扇区
int ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    if (!buffer || !g_ata_ready) return ATA_ERR_INVAL;

    for (int retry = 0; retry < 3; retry++) {
        if (ata_wait_ready_strict() != ATA_OK) continue;

        outb(0x1F6, 0xE0 | (g_ata_slave << 4) | ((lba >> 24) & 0x0F));
        outb(0x1F2, 1);
        outb(0x1F3, (uint8_t)lba);
        outb(0x1F4, (uint8_t)(lba >> 8));
        outb(0x1F5, (uint8_t)(lba >> 16));
        outb(0x1F7, 0x30); // WRITE

        uint32_t timeout = 1000000;
        int ready = 0;
        while (timeout--) {
            uint8_t status = inb(0x1F7);
            if (status & 0x01) break; // 报错
            if (!(status & 0x80) && (status & 0x08)) { ready = 1; break; } // DRQ 就绪
        }
        if (!ready) continue;

        const uint16_t *ptr = (const uint16_t*)buffer;
        for (int i = 0; i < 256; i++) {
            outw(0x1F0, *ptr++);
        }

        // ★★★ 最致命的一步：写完后，必须死等 BSY 清零，宣告这个扇区彻底写完 ★★★
        timeout = 1000000;
        while (timeout--) {
            uint8_t status = inb(0x1F7);
            if (status & 0x01) break;
            if (!(status & 0x80)) { 
                // 扇区写完了，赶紧发 FLUSH CACHE 强制落盘
                outb(0x1F7, 0xE7); 
                
                // 等待 FLUSH 彻底完成
                timeout = 1000000;
                while (timeout--) {
                    status = inb(0x1F7);
                    if (status & 0x01) break;
                    if (!(status & 0x80)) return ATA_OK; // 完美成功
                }
                break;
            }
        }
    }
    return ATA_ERR_TIMEOUT;
}

// 防御性辅助工具
const char* ata_get_error_string(int err_code) {
    switch (err_code) {
        case ATA_OK:          return "OK";
        case ATA_ERR_TIMEOUT: return "HARDWARE TIMEOUT";
        case ATA_ERR_IO:      return "I/O ERROR";
        case ATA_ERR_NOTRDY:  return "DRIVE NOT READY";
        case ATA_ERR_INVAL:   return "INVALID PARAMETER (NULL PTR)";
        default:              return "UNKNOWN ERROR";
    }
}

int ata_flush_cache(void) {
    if (!g_ata_ready) return ATA_ERR_NOTRDY;
    outb(0x1F7, 0xE7);                 /* FLUSH CACHE */
    if (ata_wait_bsy() != ATA_OK) return ATA_ERR_TIMEOUT;
    if (inb(0x1F7) & 0x01) return ATA_ERR_IO;   /* 必须检查 ERR */
    return ATA_OK;
}