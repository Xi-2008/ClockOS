#include <stdint.h>
#include "ata.h"

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

static inline void lidt(void *base, uint16_t limit) {
    struct {
        uint16_t limit;
        uint32_t base;
    } __attribute__((packed)) idtr = { limit, (uint32_t)base };
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

struct interrupt_frame {
    uint32_t ip;
    uint32_t cs;
    uint32_t flags;
    uint32_t sp;
    uint32_t ss;
} __attribute__((packed));

struct boot_info_t {
    uint8_t  drive_number;
    uint8_t  boot_type;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t _pad;
    uint32_t kernel_lba;
    uint32_t kernel_size;
    uint32_t kernel_load;
    uint32_t magic;
} __attribute__((packed));


struct bcb_t {
    uint32_t magic;       
    uint32_t active_lba;   
    uint32_t kernel_size;  
    uint32_t boot_success;  
} __attribute__((packed));


static inline void sti(void) { __asm__ volatile ("sti"); }
static inline void cli(void) { __asm__ volatile ("cli"); }
static inline void hlt(void) { __asm__ volatile ("hlt"); }

/* 磁盘布局（LBA，每扇区 512B）
 * 0        : boot
 * 1..16    : loader
 * 17..144  : kernel (loader.asm 里 KERNEL_LBA=17, KERNEL_SECS=128)
 * 145      : superblock
 * 146..161 : inode 表 (16 扇区 = 8192B = 64 个 128B inode，正好对上 MAX_FILES=64)
 * 162..163 : 空闲块位图 (2 扇区)
 * 164..    : 数据区
 */
#define FS_MAGIC_B0 'C'
#define FS_MAGIC_B1 'L'
#define FS_MAGIC_B2 'O'
#define FS_MAGIC_B3 'C'
#define FS_VERSION     1u
#define FS_MAX_INODES  (FS_INODE_SECS * 512 / 128)   // = 64，和 MAX_FILES 一致  
#define BCB_LBA        17
#define SLOT_MAIN_LBA  18    
#define SLOT_STAG_LBA  146    
#define MAX_KERNEL_SECS 128    
#define FS_SUPER_LBA   274  
#define FS_INODE_LBA   275
#define FS_INODE_SECS  16
#define FS_BITMAP_LBA  291
#define FS_BITMAP_SECS 2
#define FS_DATA_LBA    293
#define BOOT_INFO_ADDR   0x0500
#define BOOT_INFO_MAGIC  0x424F4F54
#define VGA_TEXT 0xB8000
#define VGA_GFX  0xA0000
#define BUF_SIZE 128
#define MAX_FILES 64
#define MAX_FILENAME 32
#define MAX_FILE_CONTENT (12 * 512 + 128 * 512)
#define HEAP_START 0x200000
#define HEAP_END 0x400000
#define HEAP_SIZE (HEAP_END - HEAP_START)
#define SCRIPT_VARS 16
#define SCRIPT_VAR_LEN 32
#define DEV_OK          0// 设备状态码
#define DEV_ERR_TIMEOUT 1
#define DEV_ERR_IO      2
#define DEV_ERR_NOTRDY  3

static volatile struct boot_info_t *g_boot_info =
        (struct boot_info_t *)BOOT_INFO_ADDR;

char script_vars[SCRIPT_VARS][SCRIPT_VAR_LEN];
char script_names[SCRIPT_VARS][SCRIPT_VAR_LEN];
int script_var_count = 0;

uint16_t cursor_x = 0;
uint16_t cursor_y = 0;
uint8_t last_scancode = 0;
uint8_t feedback_eoi = ' ';
uint8_t temp_char = 0;
uint16_t shift_state = 0;
uint8_t display_mode = 0;



uint8_t key_buffer[BUF_SIZE];
uint16_t buf_head = 0;
uint16_t buf_tail = 0;
uint16_t buf_len = 0;

extern uint32_t system_ticks;
uint8_t text_color = 0x0F;
void delay_loop(uint32_t count);

char history[10][128];
int history_count = 0;
int history_index = -1;

uint8_t is_extended = 0;
uint8_t caps_lock = 0;
uint8_t ctrl_state = 0;

uint8_t* edit_buffer = 0;
uint32_t edit_len = 0;
uint32_t edit_cursor = 0;
int edit_mode = 0;

const char msg_booting[] = "Booting ClockOS...";
const char msg_step1[]   = "ClockOS hasbeen successfully loaded...";
const char msg_step2[]   = "ClockOS is runing in the C environment.";
const char msg_step3[]   = "Welcome to the ClockOS. If you need help, type 'help' and press Enter.";
const char msg_step4[]   = "(C) Xi 2026 - Clock Operating System.";
const char msg_prompt[]  = "ClockOS >";
const char msg_unknown[] = "Unknown command";
const char msg_help[]    = "Commands:\nclear: Clear the screen.\ngfx:   Show terminal graphics.\ntext:  Rester\n";
const char msg_exc[]     = "EXC";

const uint8_t key_map[] = {
    0, 0, '1','2','3','4','5','6','7','8','9','0','-','=', 0, 0,
    'q','w','e','r','t','y','u','i','o','p','[',']', 0, 0, 'a','s',
    'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
    'b','n','m',',','.','/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
#define KEY_MAP_SIZE (sizeof(key_map))

const uint8_t key_map_shift[] = {
    0, 0, '!','@','#','$','%','^','&','*','(',')','_','+', 0, 0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 0, 0, 'A','S',
    'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
    'B','N','M','<','>','?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

const uint8_t vga_gfx_regs[] = {
    0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

const uint8_t img_raw[48 * 48] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x0F,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x0F,0x0F,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x0F,0x00,0x00,
    0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
    0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x00
};

int IStrncmp(const char *s1, const char *s2, unsigned int n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

void delay_loop(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; i++);
}

uint32_t str_to_int(const char *str) {
    uint32_t res = 0;
    int base = 10;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        base = 16; str += 2;
    }
    while (*str) {
        char c = *str;
        if (c >= '0' && c <= '9') c -= '0';
        else if (base == 16 && c >= 'a' && c <= 'f') c = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') c = c - 'A' + 10;
        else break;
        res = res * base + c;
        str++;
    }
    return res;
}

void int_to_str(uint32_t num, char* buf) {
    if (num == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char temp[16]; int i = 0;
    while (num > 0) { temp[i++] = (num % 10) + '0'; num /= 10; }
    for (int j = 0; j < i; j++) buf[j] = temp[i - 1 - j];
    buf[i] = 0;
}

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_entry idt[256];
struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_descriptor;

void fs_sync_to_disk(void);
int  fs_mount(void);
void fs_format(void);
void clear_screen(void);
void print_string(const char *str, uint16_t *vidptr);
void print_line(const char *str);
void set_vga_text(void);
void set_vga_gfx(void);
void scroll_screen(void);
void init_pic(void);
void init_idt(void);
void update_hardware_cursor(void);
void update_feedback(void);
uint16_t *get_cursor_pos(void);
void inc_cursor(void);
void dec_cursor(void);
void draw_pixel(uint16_t x, uint16_t y, uint8_t color);
void kmain(void);

#define CLOCKOS_VERSION "0.5.3"
#define CLOCKOS_DATE "2026-05-3"

void execute_command(const char* cmd);
void cmd_echo(const char* args);
void cmd_color(const char* args);
void cmd_calc(const char* args);
void cmd_peek(const char* args);
void cmd_poke(const char* args);
void cmd_create(const char* args);
void cmd_write(const char* args);
void cmd_cat(const char* args);
void cmd_rm(const char* args);
void cmd_edit(const char* args);
void cmd_run(const char* args);
void cmd_cp(const char* args);
void cmd_mv(const char* args);
void cmd_append(const char* args);
void cmd_update(void);
void cmd_help();
void cmd_clear();
void cmd_ls();
void cmd_gfx();
void cmd_text();
void cmd_version();
void cmd_ticks();
void cmd_mem();
void cmd_cpu();
void cmd_sysinfo();
void cmd_time();
void cmd_date();
void cmd_beep();
void cmd_reboot();
void cmd_shutdown();

void fs_sync_to_disk();
void edit_redraw();

typedef struct {
    const char* name;
    const char* desc;
    void (*func)();
} command_t;

const command_t commands[] = {
    {"help",     "Show this help message", cmd_help},
    {"clear",    "Clear the screen", cmd_clear},
    {"version",  "Show OS version", cmd_version},
    //{"echo",     "Echo your input", cmd_echo},
    {"text",     "Switch back to text mode", cmd_text},
    {"gfx",      "Switch to graphics mode", cmd_gfx},
    {"ticks",    "Show system ticks since boot", cmd_ticks},
    {"mem",      "Show memory info", cmd_mem},
    {"cpu",      "Show CPU vendor info", cmd_cpu},
    //{"color",    "Change text color (e.g., color 0x0A)", cmd_color},
    {"ls",       "List files", cmd_ls},
    {"cls",      "Alias for clear", cmd_clear},
    {"sysinfo",  "Show system summary", cmd_sysinfo},
    {"time",     "Show real time", cmd_time},
    {"date",     "Show real date", cmd_date},
    {"beep",     "Make a beep", cmd_beep},
    //{"update", "Update system from file", cmd_update},
    {"reboot",   "Reboot the system", cmd_reboot},
    {"shutdown", "Shutdown the system", cmd_shutdown},
};
const int cmd_count = sizeof(commands) / sizeof(command_t);

void execute_command(const char* cmd) {
        if (IStrncmp(cmd, "echo ", 5) == 0) {
        cmd_echo(cmd + 5);
        return;
    }

    if (IStrncmp(cmd, "color ", 6) == 0) {
        cmd_color(cmd + 6);
        return;
    }

    if (IStrncmp(cmd, "calc ", 5) == 0) {
        cmd_calc(cmd + 5);
        return;
    }

    if (IStrncmp(cmd, "peek ", 5) == 0) {
        cmd_peek(cmd + 5);
        return;
    }

    if (IStrncmp(cmd, "poke ", 5) == 0) {
        cmd_poke(cmd + 5);
        return;
    }

    if (IStrncmp(cmd, "create ", 7) == 0) { 
        cmd_create(cmd + 7); 
        return; 
    }

    if (IStrncmp(cmd, "update ", 6) == 0) { 
        cmd_update(); 
        return; 
    }
    
    if (IStrncmp(cmd, "write ", 6) == 0) { 
        cmd_write(cmd + 6); 
        return; 
    }
    
    if (IStrncmp(cmd, "cat ", 4) == 0) { 
        cmd_cat(cmd + 4); 
        return; 
    }
    
    if (IStrncmp(cmd, "rm ", 3) == 0) { 
        cmd_rm(cmd + 3); 
        return; 
    }
    
    if (IStrncmp(cmd, "edit ", 5) == 0) { 
        cmd_edit(cmd + 5); 
        return; 
    }
    
    if (IStrncmp(cmd, "run ", 4) == 0) { 
        cmd_run(cmd + 4); 
        return; 
    }

    if (IStrncmp(cmd, "cp ", 3) == 0) { 
        cmd_cp(cmd + 3); 
        return; 
    }

    if (IStrncmp(cmd, "mv ", 3) == 0) { 
        cmd_mv(cmd + 3); 
        return; 
    }
    
    if (IStrncmp(cmd, "append ", 7) == 0) { 
        cmd_append(cmd + 7); 
        return; 
    }
    
    if (IStrncmp(cmd, "set ", 4) == 0) { 
        cmd_run(cmd); 
        return; 
    }
    
    if (IStrncmp(cmd, "loop ", 5) == 0) { 
        cmd_run(cmd); 
        return; 
    }

    if (IStrncmp(cmd, "endloop", 7) == 0) { 
        return; 
    }
    
    if (IStrncmp(cmd, "if ", 3) == 0) { 
        cmd_run(cmd); return; 
    }

    if (IStrncmp(cmd, "endif", 5) == 0) { 
        return; 
    }

    int len = 0;
    while (cmd[len]) len++;

    for (int i = 0; i < cmd_count; i++) {
        int clen = 0;
        while (commands[i].name[clen]) clen++;
        
        if (len == clen && IStrncmp(cmd, commands[i].name, len) == 0) {
            commands[i].func(); 
            return;
        }
    }
    print_line("Unknown command. Type 'help'.");
}

int get_str_len(const char* str) {
    int len = 0;
    while(str[len]) len++;
    return len;
}

typedef struct {
    char name[MAX_FILENAME];
    uint8_t *data;
    uint32_t size;
    uint32_t create_tick;
    uint8_t used;
} file_node_t;

file_node_t file_system[MAX_FILES];
uint32_t heap_used = 0;

struct superblock {
    uint32_t magic;       
    uint32_t version;       
    uint32_t block_size;   
    uint32_t total_blocks;
    uint32_t inode_count;    
    uint32_t inode_start_lba;  
    uint32_t inode_blocks;  
    uint32_t bitmap_start_lba; 
    uint32_t bitmap_blocks; 
    uint32_t data_start_lba;   
    uint8_t  reserved[472];   
} __attribute__((packed));

struct inode {
    uint16_t type;     
    uint16_t mode;      
    uint32_t size;        
    uint32_t blocks[12];    
    uint32_t indirect_block;
    uint8_t  name[32];      
    uint8_t  reserved[36];  
} __attribute__((packed));

void* kalloc(uint32_t size) {
    size = (size + 3) & ~3;
    if (heap_used + size > HEAP_SIZE) return 0;
    void* ptr = (void*)(HEAP_START + heap_used);
    heap_used += size;
    return ptr;
}

void kfree(uint8_t* ptr, uint32_t size) {
    if (!ptr) return;
    uint32_t aligned_size = (size + 3) & ~3;
    uint32_t addr = (uint32_t)ptr;
    if (addr >= HEAP_START && addr + aligned_size <= HEAP_START + heap_used) {
        if (addr + aligned_size == HEAP_START + heap_used) {
            heap_used -= aligned_size;
        } else {
            uint8_t* dest = ptr;
            uint8_t* src = (uint8_t*)(addr + aligned_size);
            uint32_t len = heap_used - (addr - HEAP_START) - aligned_size;
            for (uint32_t i = 0; i < len; i++) dest[i] = src[i];
            heap_used -= aligned_size;
            for (int i = 0; i < MAX_FILES; i++) {
                if (file_system[i].used && file_system[i].data >= ptr && file_system[i].data < ptr + len) {
                    file_system[i].data -= aligned_size;
                }
            }
        }
    }
}

int str_cmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

void str_cpy(char* dest, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dest[i] = src[i]; i++; }
    dest[i] = 0;
}

int find_file(const char* name) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].used && str_cmp(file_system[i].name, name) == 0) return i;
    }
    return -1;
}

int alloc_file_slot() {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_system[i].used) return i;
    }
    return -1;
}

void fs_init(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        file_system[i].used = 0;
        file_system[i].name[0] = 0;
        file_system[i].data = 0;
        file_system[i].size = 0;
        file_system[i].create_tick = 0;
    }
    heap_used = 0; // 重置堆分配指针
}
void cmd_ls() {
    print_line("=== File System ===");
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].used) {
            print_string(file_system[i].name, get_cursor_pos());
            cursor_x += get_str_len(file_system[i].name);
            while (cursor_x < 20) {
                uint16_t* vid = get_cursor_pos();
                *vid = ' ' | (text_color << 8);
                cursor_x++;
            }
            char buf[16];
            int_to_str(file_system[i].size, buf);
            print_string(buf, get_cursor_pos());
            cursor_x += get_str_len(buf);
            print_line(" bytes");
        }
    }
}

void cmd_update(void) {
    print_line("=== System Update from Disk 1 (Slave) ===");
    
    static uint8_t buf[512];
    uint32_t sectors_to_write = 128;

    print_line("[1/2] Reading from Secondary Disk...");

    g_ata_slave = 1; 
    
    if (ata_read_sector(0, buf) != ATA_OK) {
        print_line("Error: Cannot read Slave Disk.");
        g_ata_slave = 0; 
        return;
    }
    g_ata_slave = 0;

    print_line("[2/2] Flashing to Main Area (LBA 18)...");

    for (uint32_t s = 0; s < sectors_to_write; s++) {
        g_ata_slave = 1;
        ata_read_sector(s, buf);   
        g_ata_slave = 0;
        
        if (ata_write_sector(SLOT_MAIN_LBA + s, buf) != ATA_OK) {
            print_line("Flash FAILED!");
            return;
        }
    }

    static struct bcb_t bcb;
    bcb.magic = 0x42434200;
    bcb.active_lba = SLOT_MAIN_LBA; 
    bcb.kernel_size = sectors_to_write;
    bcb.boot_success = 0;

    ata_write_sector(BCB_LBA, (uint8_t*)&bcb);
    ata_flush_cache();
    
    print_line("Update Complete! Rebooting...");
    for(volatile int i=0; i<50000000; i++);
    cmd_reboot();
}

void cmd_create(const char* args) {
    if (args[0] == 0) {
        print_line("Usage: create [filename]");
        return;
    }
    if (find_file(args) != -1) {
        print_line("File already exists.");
        return;
    }
    int slot = alloc_file_slot();
    if (slot == -1) {
        print_line("File system full.");
        return;
    }
    str_cpy(file_system[slot].name, args, MAX_FILENAME);
    file_system[slot].data = 0;
    file_system[slot].size = 0;
    file_system[slot].create_tick = system_ticks;
    file_system[slot].used = 1;
    fs_sync_to_disk();
    print_line("File created.");
}

void cmd_write(const char* args) {
    char name[MAX_FILENAME];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < MAX_FILENAME - 1) {
        name[i] = args[i];
        i++;
    }
    name[i] = 0;
    if (args[i] == 0) {
        print_line("Usage: write [filename] [content]");
        return;
    }
    const char* content = args + i + 1;
    int slot = find_file(name);
    if (slot == -1) {
        slot = alloc_file_slot();
        if (slot == -1) {
            print_line("File system full.");
            return;
        }
        str_cpy(file_system[slot].name, name, MAX_FILENAME);
        file_system[slot].create_tick = system_ticks;
        file_system[slot].data = 0;
        file_system[slot].size = 0;
        file_system[slot].used = 1;
    } else {
        if (file_system[slot].data) {
            kfree(file_system[slot].data, file_system[slot].size);
            file_system[slot].data = 0;
            file_system[slot].size = 0;
        }
    }
    int len = get_str_len(content);
    if (len == 0) {
        print_line("Saved empty file.");
        return;
    }
    if (len > MAX_FILE_CONTENT) {
        print_line("File too large.");
        return;
    }
    uint8_t* buf = (uint8_t*)kalloc(len + 1);
    if (!buf) {
        print_line("Disk full. Write failed.");
        return;
    }
    for (int j = 0; j < len; j++) buf[j] = content[j];
    buf[len] = 0;
    file_system[slot].data = buf;
    file_system[slot].size = len;
    fs_sync_to_disk();
    print_line("Written successfully.");
}

void cmd_cat(const char* args) {
    if (args[0] == 0) {
        print_line("Usage: cat [filename]");
        return;
    }
    int slot = find_file(args);
    if (slot == -1) {
        print_line("File not found.");
        return;
    }
    if (!file_system[slot].data || file_system[slot].size == 0) {
        print_line("File is empty.");
        return;
    }
    char* p = (char*)file_system[slot].data;
    for (uint32_t i = 0; i < file_system[slot].size; i++) {
        if (p[i] == '\n') {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= 25) { cursor_y = 24; scroll_screen(); }
        } else {
            uint16_t* vid = get_cursor_pos();
            *vid = (uint16_t)p[i] | (text_color << 8);
            inc_cursor();
        }
    }
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= 25) { cursor_y = 24; scroll_screen(); }
    update_hardware_cursor();
}

void cmd_cp(const char* args) {
    char src[MAX_FILENAME], dst[MAX_FILENAME];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && i < MAX_FILENAME - 1) src[i] = args[i++];
    src[i] = 0;
    if (args[i] == 0) { print_line("Usage: cp [src] [dst]"); return; }
    i++;
    while (args[i] && j < MAX_FILENAME - 1) dst[j++] = args[i++];
    dst[j] = 0;
    
    int s = find_file(src);
    if (s == -1) { print_line("Source not found."); return; }
    if (find_file(dst) != -1) { print_line("Destination exists."); return; }
    
    int d = alloc_file_slot();
    if (d == -1) { print_line("File system full."); return; }
    
    str_cpy(file_system[d].name, dst, MAX_FILENAME);
    file_system[d].size = file_system[s].size;
    file_system[d].create_tick = system_ticks;
    file_system[d].used = 1;
    if (file_system[s].size > 0) {
        file_system[d].data = (uint8_t*)kalloc(file_system[s].size + 1);
        if (!file_system[d].data) { file_system[d].used = 0; print_line("Disk full."); return; }
        for (uint32_t k = 0; k < file_system[s].size; k++) file_system[d].data[k] = file_system[s].data[k];
        file_system[d].data[file_system[s].size] = 0;
    } else {
        file_system[d].data = 0;
    }
    fs_sync_to_disk();
    print_line("Copied.");
}

void cmd_mv(const char* args) {
    char src[MAX_FILENAME], dst[MAX_FILENAME];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && i < MAX_FILENAME - 1) src[i] = args[i++];
    src[i] = 0;
    if (args[i] == 0) { print_line("Usage: mv [src] [dst]"); return; }
    i++;
    while (args[i] && j < MAX_FILENAME - 1) dst[j++] = args[i++];
    dst[j] = 0;
    
    int s = find_file(src);
    if (s == -1) { print_line("Source not found."); return; }
    if (find_file(dst) != -1) { print_line("Destination exists."); return; }
    
    str_cpy(file_system[s].name, dst, MAX_FILENAME);
    fs_sync_to_disk();
    print_line("Renamed.");
}

void cmd_append(const char* args) {
    char name[MAX_FILENAME];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < MAX_FILENAME - 1) name[i] = args[i++];
    name[i] = 0;
    if (args[i] == 0) { print_line("Usage: append [name] [content]"); return; }
    const char* content = args + i + 1;
    int len = get_str_len(content);
    if (len == 0) { print_line("Nothing to append."); return; }
    
    int slot = find_file(name);
    if (slot == -1) { print_line("File not found. Use create."); return; }
    
    uint32_t old_size = file_system[slot].size;
    if (old_size + len + 1 > MAX_FILE_CONTENT) { print_line("File too large."); return; }
    
    uint8_t* new_data = (uint8_t*)kalloc(old_size + len + 1);
    if (!new_data) { print_line("Disk full."); return; }
    
    for (uint32_t k = 0; k < old_size; k++) new_data[k] = file_system[slot].data[k];
    for (int k = 0; k < len; k++) new_data[old_size + k] = content[k];
    new_data[old_size + len] = 0;
    
    if (file_system[slot].data) kfree(file_system[slot].data, old_size);
    file_system[slot].data = new_data;
    file_system[slot].size = old_size + len;
    fs_sync_to_disk();
    print_line("Appended.");
}

void cmd_rm(const char* args) {
    if (args[0] == 0) {
        print_line("Usage: rm [filename]");
        return;
    }
    int slot = find_file(args);
    if (slot == -1) {
        print_line("File not found.");
        return;
    }
    if (file_system[slot].data) {
        kfree(file_system[slot].data, file_system[slot].size);
    }
    file_system[slot].used = 0;
    file_system[slot].data = 0;
    file_system[slot].size = 0;
    fs_sync_to_disk();
    print_line("File removed.");
}



void cmd_edit(const char* args) {
    if (!edit_buffer) {
        edit_buffer = (uint8_t*)kalloc(65536);
        if (!edit_buffer) { print_line("Out of memory."); return; }
    }
    char filename[MAX_FILENAME];
    if (args[0] == 0) str_cpy(filename, "untitled.txt", MAX_FILENAME);
    else str_cpy(filename, args, MAX_FILENAME);
    
    int slot = find_file(filename);
    if (slot == -1) {
        slot = alloc_file_slot();
        if (slot == -1) { print_line("File system full."); return; }
        str_cpy(file_system[slot].name, filename, MAX_FILENAME);
        file_system[slot].create_tick = system_ticks;
        file_system[slot].data = 0;
        file_system[slot].size = 0;
        file_system[slot].used = 1;
    } else {
        edit_len = file_system[slot].size;
        for (uint32_t i = 0; i < edit_len; i++) edit_buffer[i] = file_system[slot].data[i];
        edit_buffer[edit_len] = 0;
    }
    
    if (slot != -1 && file_system[slot].size == 0) edit_len = 0;
    edit_cursor = 0;
    edit_mode = 1;
    edit_redraw();
    
    uint8_t last_sc = 0;
    cli();
    while (edit_mode) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (sc == last_sc) continue;
            last_sc = sc;
            
            if (sc == 0xE0) { // 扩展键前缀
                while (!(inb(0x64) & 1));
                sc = inb(0x60);
                if (sc == 0x48) { // 上
                    uint32_t c = edit_cursor;
                    while (c > 0 && edit_buffer[c - 1] != '\n') c--;
                    if (c > 0) c--;
                    while (c > 0 && edit_buffer[c - 1] != '\n') c--;
                    edit_cursor = c;
                } else if (sc == 0x50) { // 下
                    uint32_t c = edit_cursor;
                    while (c < edit_len && edit_buffer[c] != '\n') c++;
                    if (c < edit_len) c++;
                    while (c < edit_len && edit_buffer[c] != '\n') c++;
                    edit_cursor = c;
                } else if (sc == 0x4B) { // 左
                    if (edit_cursor > 0) edit_cursor--;
                } else if (sc == 0x4D) { // 右
                    if (edit_cursor < edit_len) edit_cursor++;
                }
                edit_redraw();
                continue;
            }
            sti();
            
            if (sc == 0x01) { edit_mode = 0; break; }
            if (sc == 0x2A || sc == 0x36) { shift_state = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift_state = 0; continue; }
            if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
            
            if (sc & 0x80) {
                if (sc == 0x8E && edit_cursor > 0) {
                    for (uint32_t i = edit_cursor; i < edit_len; i++) edit_buffer[i - 1] = edit_buffer[i];
                    edit_len--; edit_cursor--;
                    edit_redraw();
                }
                continue;
            }
            
            if (sc == 0x1C) {
                if (edit_len < 65535) {
                    for (uint32_t i = edit_len; i > edit_cursor; i--) edit_buffer[i] = edit_buffer[i - 1];
                    edit_buffer[edit_cursor] = '\n';
                    edit_len++; edit_cursor++;
                    edit_redraw();
                }
            } else if (sc == 0x0E) {
                if (edit_cursor > 0) {
                    for (uint32_t i = edit_cursor; i < edit_len; i++) edit_buffer[i - 1] = edit_buffer[i];
                    edit_len--; edit_cursor--;
                    edit_redraw();
                }
            } else if (sc == 0x0F) {
                for (int t = 0; t < 4; t++) {
                    if (edit_len < 65535) {
                        for (uint32_t i = edit_len; i > edit_cursor; i--) edit_buffer[i] = edit_buffer[i - 1];
                        edit_buffer[edit_cursor] = ' ';
                        edit_len++; edit_cursor++;
                    }
                }
                edit_redraw();
            } else if (sc < KEY_MAP_SIZE) {
                uint8_t ascii = 0;
                int is_upper = 0;
                if (shift_state) is_upper = 1;
                if (caps_lock && (sc >= 0x10 && sc <= 0x32)) is_upper = !is_upper;
                if (is_upper) ascii = key_map_shift[sc];
                else ascii = key_map[sc];
                if (ascii != 0 && edit_len < 65535) {
                    for (uint32_t i = edit_len; i > edit_cursor; i--) edit_buffer[i] = edit_buffer[i - 1];
                    edit_buffer[edit_cursor] = ascii;
                    edit_len++; edit_cursor++;
                    edit_redraw();
                }
            }
        }
    }
    
    if (file_system[slot].data) kfree(file_system[slot].data, file_system[slot].size);
    file_system[slot].data = (uint8_t*)kalloc(edit_len + 1);
    if (file_system[slot].data) {
        for (uint32_t i = 0; i < edit_len; i++) file_system[slot].data[i] = edit_buffer[i];
        file_system[slot].data[edit_len] = 0;
        file_system[slot].size = edit_len;
    }
    clear_screen();
    fs_sync_to_disk();
    print_line("Saved.");
}

void cmd_run(const char* args) {
    if (args[0] == 0) { print_line("Usage: run [filename.clo]"); return; }
    int slot = find_file(args);
    if (slot == -1) { print_line("File not found."); return; }
    if (!file_system[slot].data || file_system[slot].size == 0) { print_line("File is empty."); return; }
    
    char* script = (char*)file_system[slot].data;
    uint32_t i = 0;
    uint32_t line_start = 0;
    int loop_start = -1;
    int loop_count = 0;
    int if_state = 0;
    
    while (i <= file_system[slot].size) {
        if (script[i] == '\n' || script[i] == 0) {
            uint32_t line_len = i - line_start;
            if (line_len > 0) {
                char line[128];
                uint32_t c = 0;
                for (uint32_t k = 0; k < line_len && c < 127; k++) line[c++] = script[line_start + k];
                line[c] = 0;
                
                if (IStrncmp(line, "set ", 4) == 0) {
                    char var_name[SCRIPT_VAR_LEN], var_val[SCRIPT_VAR_LEN];
                    int vi = 0, vj = 0;
                    uint32_t k = 4;
                    while (line[k] && line[k] != ' ' && vi < SCRIPT_VAR_LEN - 1) var_name[vi++] = line[k++];
                    var_name[vi] = 0;
                    if (line[k] == ' ') k++;
                    while (line[k] && vj < SCRIPT_VAR_LEN - 1) var_val[vj++] = line[k++];
                    var_val[vj] = 0;
                    
                    int found = -1;
                    for (int v = 0; v < script_var_count; v++) {
                        if (str_cmp(script_names[v], var_name) == 0) { found = v; break; }
                    }
                    if (found == -1 && script_var_count < SCRIPT_VARS) {
                        found = script_var_count++;
                        str_cpy(script_names[found], var_name, SCRIPT_VAR_LEN);
                    }
                    if (found != -1) str_cpy(script_vars[found], var_val, SCRIPT_VAR_LEN);
                } else if (IStrncmp(line, "loop ", 5) == 0) {
                    loop_start = i;
                    loop_count = str_to_int(line + 5);
                } else if (IStrncmp(line, "endloop", 7) == 0) {
                    if (loop_count > 1) {
                        loop_count--;
                        i = loop_start;
                        line_start = i + 1;
                        continue;
                    } else {
                        loop_count = 0;
                    }
                } else if (IStrncmp(line, "if ", 3) == 0) {
                    if_state = 1;
                    char var_name[SCRIPT_VAR_LEN], var_val[SCRIPT_VAR_LEN];
                    int vi = 0, vj = 0;
                    uint32_t k = 3;
                    while (line[k] && line[k] != ' ' && line[k] != '=' && vi < SCRIPT_VAR_LEN - 1) var_name[vi++] = line[k++];
                    var_name[vi] = 0;
                    while (line[k] && line[k] != '=') k++;
                    if (line[k] == '=') k++;
                    while (line[k] && line[k] != ' ' && vj < SCRIPT_VAR_LEN - 1) var_val[vj++] = line[k++];
                    var_val[vj] = 0;
                    
                    int found = -1;
                    for (int v = 0; v < script_var_count; v++) {
                        if (str_cmp(script_names[v], var_name) == 0) { found = v; break; }
                    }
                    if (found == -1 || str_cmp(script_vars[found], var_val) != 0) {
                        while (i <= file_system[slot].size) {
                            if (script[i] == '\n' || script[i] == 0) {
                                uint32_t nl = i - line_start;
                                if (nl > 0) {
                                    char l2[128];
                                    uint32_t c2 = 0;
                                    for (uint32_t k2 = 0; k2 < nl && c2 < 127; k2++) l2[c2++] = script[line_start + k2];
                                    l2[c2] = 0;
                                    if (IStrncmp(l2, "endif", 5) == 0) break;
                                }
                            }
                            i++;
                        }
                    }
                } else if (IStrncmp(line, "endif", 5) == 0) {
                    if_state = 0;
                } else {
                    char expanded[128];
                    uint32_t ei = 0;
                    for (uint32_t k = 0; line[k] && ei < 127; k++) {
                        if (line[k] == '$') {
                            k++;
                            char var_name[SCRIPT_VAR_LEN];
                            int vi = 0;
                            while (line[k] && line[k] != ' ' && line[k] != '\n' && vi < SCRIPT_VAR_LEN - 1) var_name[vi++] = line[k++];
                            var_name[vi] = 0;
                            int found = -1;
                            for (int v = 0; v < script_var_count; v++) {
                                if (str_cmp(script_names[v], var_name) == 0) { found = v; break; }
                            }
                            if (found != -1) {
                                int vl = get_str_len(script_vars[found]);
                                for (int vk = 0; vk < vl && ei < 127; vk++) expanded[ei++] = script_vars[found][vk];
                            }
                            k--;
                        } else {
                            expanded[ei++] = line[k];
                        }
                    }
                    expanded[ei] = 0;
                    if (ei > 0 && expanded[0] == 'p' && expanded[1] == 'r' && expanded[2] == 'i') {
                        print_line(expanded + 6);
                    } else {
                        execute_command(expanded);
                    }
                }
            }
            line_start = i + 1;
        }
        i++;
    }
}

void help_print_paged(const char* title, const char* lines[], int count) {
    print_line(title);
    for (int i = 0; i < count; i++) {
        print_line(lines[i]);
        if ((i + 2) % 22 == 0) {
            print_string("-- More -- (Press any key)", (uint16_t*)(VGA_TEXT + 24 * 80 * 2));
            while (!(inb(0x64) & 1));
            inb(0x60);
            for (int j = 0; j < 80; j++) ((uint16_t*)(VGA_TEXT + 24 * 80 * 2))[j] = 0x0720;
        }
    }
}

void cmd_help() {
    const char* h1[] = {
        "=== ClockOS v0.5 Help ===",
        "",
        "File System Commands:",
        "  ls                 - List all files",
        "  create [name]      - Create an empty file",
        "  write [name] [txt] - Write content to a file",
        "  cat [name]         - Read a file's content",
        "  append [name] [txt]- Append content to a file",
        "  cp [src] [dst]     - Copy a file",
        "  mv [src] [dst]     - Rename a file",
        "  rm [name]          - Remove a file",
        "  edit [name]        - Open full-screen editor",
        "  run [name.clo]     - Run a script file",
        "",
        "System Commands:",
        "  help               - Show this message",
        "  clear / cls        - Clear the screen",
        "  version            - Show OS version",
        "  sysinfo            - Show system summary",
        "  time / date        - Show real time/date",
        "  ticks              - Show system ticks",
        "  cpu                - Show CPU vendor",
        "  mem                - Show memory info",
        "  color [hex]        - Change text color",
        "  beep               - Make a beep",
        "  reboot             - Reboot system",
        "  shutdown           - Shutdown system"
    };
    help_print_paged("=== ClockOS Help (Page 1/1) ===", h1, sizeof(h1)/sizeof(h1[0]));
}

void cmd_version() {
    print_line("ClockOS Version: " CLOCKOS_VERSION);
    print_line("Build Date: " CLOCKOS_DATE);
    print_line("Arch: x86 32-bit Protected Mode");
}

void cmd_echo(const char* args) {
    if (args[0] == 0) {
        print_line("Echo: (nothing)");
    } else {
        print_line(args);
    }
}

void cmd_text() {
    if (display_mode == 0) {
        print_line("Already in text mode.");
    } else {
        display_mode = 0;
        set_vga_text();
        clear_screen();
    }
}

void cmd_clear() {
    clear_screen();
}

void cmd_gfx() {
    cli();
    set_vga_gfx();
    display_mode = 1;
    uint8_t *gfx = (uint8_t *)VGA_GFX;
    for (uint32_t i = 0; i < 320 * 200; i++) {
        gfx[i] = 1; 
    }
    uint32_t offset = (320 * 76 + 136); 
    for (int row = 0; row < 48; row++) {
        for (int col = 0; col < 48; col++) {
            uint8_t pix = img_raw[row * 48 + col];
            
            if (pix != 0x00) {
                gfx[offset + row * 320 + col] = pix;
            }
        }
    }
    int exit_gfx = 0;
    while (!exit_gfx) {
        if (inb(0x64) & 1) {
            uint8_t sc = inb(0x60);
            if (sc == 0x01) {
                exit_gfx = 1;
            }
        }
    }
    set_vga_text();
    display_mode = 0;
    clear_screen();
    sti();
    print_line("Returned from GFX mode.");
}

void cmd_ticks() {
    extern uint32_t system_ticks; 
    print_string("System Ticks: 0x", get_cursor_pos());
    uint32_t t = system_ticks;
    for(int i=28; i>=0; i-=4) {
        uint8_t nibble = (t >> i) & 0xF;
        uint16_t* vid = get_cursor_pos();
        *vid = (nibble < 10 ? '0' + nibble : 'A' + nibble - 10) | 0x0F00;
        cursor_x++;
    }
    print_line("");
}

void cmd_mem() {
    print_line("Memory Map:");
    print_line("  Kernel loaded at 0x9000");
    print_line("  Stack at 0x90000");
    print_line("  Video RAM at 0xB8000");
    print_line("  E820 map: Not implemented yet.");
}

void cmd_cpu() {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;
    print_string("CPU Vendor: ", get_cursor_pos());
    print_line(vendor);
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}

void cmd_reboot() {
    print_line("Syncing disk...");
    // 强制全量写盘并 flush
    fs_sync_to_disk(); 
    
    // 强行延时，给 QEMU 宿主机一点时间把缓存写入文件
    for (volatile int i = 0; i < 50000000; i++); 

    print_line("Rebooting...");
    while (inb(0x64) & 0x02); 
    outb(0x64, 0xFE);         
    while(1) hlt();
}

void cmd_time() {
    uint8_t h, m, s;
    outb(0x70, 0x04); h = inb(0x71);
    outb(0x70, 0x02); m = inb(0x71);
    outb(0x70, 0x00); s = inb(0x71);
    h = (h & 0x0F) + ((h >> 4) * 10); m = (m & 0x0F) + ((m >> 4) * 10); s = (s & 0x0F) + ((s >> 4) * 10);
    print_string("Time: ", get_cursor_pos());
    char buf[12]; buf[0] = (h / 10) + '0'; buf[1] = (h % 10) + '0'; buf[2] = ':'; 
    buf[3] = (m / 10) + '0'; buf[4] = (m % 10) + '0'; buf[5] = ':'; 
    buf[6] = (s / 10) + '0'; buf[7] = (s % 10) + '0'; buf[8] = 0;
    print_line(buf);
}

void cmd_date() {
    uint8_t y, mo, d;
    outb(0x70, 0x09); y = inb(0x71);
    outb(0x70, 0x08); mo = inb(0x71);
    outb(0x70, 0x07); d = inb(0x71);
    y = (y & 0x0F) + ((y >> 4) * 10); mo = (mo & 0x0F) + ((mo >> 4) * 10); d = (d & 0x0F) + ((d >> 4) * 10);
    print_string("Date: 20", get_cursor_pos());
    char buf[12]; buf[0] = (y / 10) + '0'; buf[1] = (y % 10) + '0'; buf[2] = '-'; 
    buf[3] = (mo / 10) + '0'; buf[4] = (mo % 10) + '0'; buf[5] = '-'; 
    buf[6] = (d / 10) + '0'; buf[7] = (d % 10) + '0'; buf[8] = 0;
    print_line(buf);
}

void cmd_calc(const char* args) {
    uint32_t n1 = 0, n2 = 0; char op = '+';
    const char* p = args;
    while (*p && *p != ' ') { p++; }
    if (*p) { p++; op = *p; p += 2; }
    n1 = str_to_int(args);
    n2 = str_to_int(p);
    uint32_t res = 0;
    if (op == '+') res = n1 + n2; else if (op == '-') res = n1 - n2;
    else if (op == '*') res = n1 * n2; else if (op == '/') res = n2 ? n1 / n2 : 0;
    
    char buf[32]; int_to_str(res, buf);
    print_string("Result: ", get_cursor_pos()); print_line(buf);
}

void cmd_peek(const char* args) {
    uint32_t addr = str_to_int(args);
    volatile uint8_t *ptr = (volatile uint8_t*)addr;
    print_string("Hex value at 0x", get_cursor_pos());
    uint8_t val = *ptr;
    print_string("Addr read done (see value below)\n", get_cursor_pos()); 
}

void cmd_poke(const char* args) {
    uint32_t addr = 0, val = 0;
    char buf[32]; int i = 0;
    while (args[i] != ' ' && args[i]) { buf[i] = args[i]; i++; }
    buf[i] = 0; addr = str_to_int(buf);
    val = str_to_int(args + i + 1);
    volatile uint8_t *ptr = (volatile uint8_t*)addr;
    *ptr = (uint8_t)val;
    print_line("Memory poked.");
}

void cmd_beep() {
    outb(0x61, inb(0x61) | 3);
    delay_loop(5000000);
    outb(0x61, inb(0x61) & 0xFC);
    print_line("Beep!");
}

void cmd_sysinfo() {
    print_line("=== System Info ===");
    cmd_version();
    cmd_cpu();
    print_line("Total ticks: ");
}

void cmd_shutdown() {
    print_line("Saving data to disk...");
    fs_sync_to_disk();
    print_line("Shutting down...");
    outw(0x604, 0x2000);  // QEMU
    outw(0x4004, 0x3400); // VirtualBox
    outw(0xB004, 0x2000); // Bochs
    outw(0x1004, 0x2000); // VMware
    cli();
    print_line("Failed to auto-shutdown.");
    while(1) hlt();
}

void cmd_color(const char* args) {
    if (args[0] == 0) {
        print_line("Usage: color [hex, e.g., 0x0A]");
        return;
    }
    text_color = (uint8_t)str_to_int(args);
    
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    for (int i = 0; i < 2000; i++) {
        vid[i] = (vid[i] & 0x00FF) | (text_color << 8);
    }
    print_line("Color updated globally.");
}

__attribute__((interrupt)) void exception_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    print_string(msg_exc, vid);
    while (1) hlt();
}

__attribute__((interrupt)) void exception_err_handler(struct interrupt_frame *frame, uint32_t error_code) {
    (void)frame;
    (void)error_code;
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    print_string(msg_exc, vid);
    while (1) hlt();
}

__attribute__((interrupt)) void double_fault_handler(struct interrupt_frame *frame, uint32_t error_code) {
    (void)frame;
    (void)error_code;
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    vid[0] = 0x4F00 | 'D';
    vid[1] = 0x4F00 | 'F';
    while (1) hlt();
}

__attribute__((interrupt)) void isr_keyboard(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb(0x60);
    last_scancode = scancode;

    if (scancode == 0xE0) { 
        is_extended = 1; 
        goto done; 
    }
    if (is_extended == 1) {
        is_extended = 0;
        
        if (scancode == 0x48) {
            if (history_count > 0 && history_index < history_count - 1) {
                history_index++;
                while (buf_len > 0) {
                    buf_len--;
                    buf_head = (buf_head - 1) & (BUF_SIZE - 1);
                    dec_cursor();
                    uint16_t *vid = get_cursor_pos();
                    *vid = 0x0720;
                }
                int i = 0;
                while (history[history_index][i]) {
                    key_buffer[buf_head] = history[history_index][i++];
                    buf_head = (buf_head + 1) & (BUF_SIZE - 1);
                    buf_len++;
                }
                for (int j = 0; j < buf_len; j++) {
                    uint16_t *vid = get_cursor_pos();
                    *vid = key_buffer[(buf_head - buf_len + j) & (BUF_SIZE - 1)] | (text_color << 8);
                    inc_cursor();
                }
            }
        } else if (scancode == 0x50) {
            if (history_index > 0) {
                history_index--;
                while (buf_len > 0) {
                    buf_len--;
                    buf_head = (buf_head - 1) & (BUF_SIZE - 1);
                    dec_cursor();
                    uint16_t *vid = get_cursor_pos();
                    *vid = 0x0720;
                }
                int i = 0;
                while (history[history_index][i]) {
                    key_buffer[buf_head] = history[history_index][i++];
                    buf_head = (buf_head + 1) & (BUF_SIZE - 1);
                    buf_len++;
                }
                for (int j = 0; j < buf_len; j++) {
                    uint16_t *vid = get_cursor_pos();
                    *vid = key_buffer[(buf_head - buf_len + j) & (BUF_SIZE - 1)] | (text_color << 8);
                    inc_cursor();
                }
            } else if (history_index == 0) {
                history_index = -1;
                while (buf_len > 0) {
                    buf_len--;
                    buf_head = (buf_head - 1) & (BUF_SIZE - 1);
                    dec_cursor();
                    uint16_t *vid = get_cursor_pos();
                    *vid = 0x0720;
                }
            }
        }
        goto done; 
    }

    if (scancode == 0x01 && display_mode == 1) {
        display_mode = 0;
        set_vga_text();
        clear_screen();
        goto done;
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_state = 1;
        goto done;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_state = 0;
        goto done;
    }

    if (scancode == 0xE0 || scancode == 0xE1 || scancode == 0xF0)
        goto done;
    if (scancode & 0x80)
        goto done;

    if (scancode == 0x0E) {
        if (display_mode == 1) goto done;
        if (buf_len == 0) goto done;
        buf_len--;
        buf_head = (buf_head - 1) & (BUF_SIZE - 1);
        dec_cursor();
        uint16_t *vid = get_cursor_pos();
        *vid = 0x0720;
        update_hardware_cursor();
        goto done;
    }

    if (scancode == 0x1C) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= 25) {
            cursor_y = 24;
            scroll_screen();
        }
        update_hardware_cursor();

        if (buf_len > 0) {
            uint16_t start = (buf_head - buf_len) & (BUF_SIZE - 1);
            uint8_t cmd[BUF_SIZE + 1];
            for (int i = 0; i < buf_len; i++)
                cmd[i] = key_buffer[(start + i) & (BUF_SIZE - 1)];
            cmd[buf_len] = 0;
            
            if (history_count < 10) {
                for (int i = 0; i < buf_len; i++) history[history_count][i] = key_buffer[i];
                history[history_count][buf_len] = 0;
                history_count++;
            } else {
                for (int i = 0; i < 9; i++) {
                    for (int j = 0; j < 128; j++) history[i][j] = history[i+1][j];
                }
                for (int i = 0; i < buf_len; i++) history[9][i] = key_buffer[i];
                history[9][buf_len] = 0;
            }
            history_index = -1;

            execute_command((char*)cmd);

            buf_head = buf_tail = buf_len = 0;
        }
        print_string(msg_prompt, get_cursor_pos());
        cursor_x += 10;
        update_hardware_cursor();
        goto done;
    }

    if (scancode == 0x0F) {
        for (int i = 0; i < cmd_count; i++) {
            int match = 1;
            for (int j = 0; j < buf_len; j++) {
                if (key_buffer[(buf_head - buf_len + j) & (BUF_SIZE - 1)] != commands[i].name[j]) {
                    match = 0; break;
                }
            }
            if (match) {
                int k = buf_len;
                while (commands[i].name[k] && buf_len < BUF_SIZE) {
                    uint16_t *v = get_cursor_pos();
                    *v = commands[i].name[k] | (text_color << 8);
                    inc_cursor();
                    key_buffer[buf_head] = commands[i].name[k];
                    buf_head = (buf_head + 1) & (BUF_SIZE - 1);
                    buf_len++;
                    k++;
                }
                update_hardware_cursor();
                break;
            }
        }
        goto done;
    }

    if (scancode == 0x3A) { caps_lock = !caps_lock; goto done; }
    if (scancode == 0x2A || scancode == 0x36) { shift_state = 1; goto done; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_state = 0; goto done; }

    if (scancode == 0xE1 || scancode == 0xF0 || (scancode & 0x80)) goto done;

    if (scancode == 0x0E) {
        if (display_mode == 1) goto done;
        if (buf_len == 0) goto done;
        buf_len--;
        buf_head = (buf_head - 1) & (BUF_SIZE - 1);
        dec_cursor();
        uint16_t *vid = get_cursor_pos();
        *vid = 0x0720;
        update_hardware_cursor();
        goto done;
    }

    if (scancode >= KEY_MAP_SIZE) goto done;
    uint8_t ascii = 0;
    
    int is_upper = 0;
    if (shift_state) is_upper = 1;
    if (caps_lock && (scancode >= 0x10 && scancode <= 0x32)) is_upper = !is_upper;
    
    if (is_upper) {
        ascii = key_map_shift[scancode];
    } else {
        ascii = key_map[scancode];
    }
    if (ascii == 0) goto done;

    key_buffer[buf_head] = ascii;
    buf_head = (buf_head + 1) & (BUF_SIZE - 1);
    buf_len++;

    uint16_t *vid = get_cursor_pos();
    *vid = (uint16_t)ascii | (text_color << 8);
    inc_cursor();
    update_hardware_cursor();

done:
    outb(0x20, 0x20);   // EOI
}

void edit_redraw() {
    clear_screen();
    uint16_t* vid = (uint16_t*)VGA_TEXT;
    uint16_t color = text_color << 8;
    for (int i = 0; i < 80; i++) vid[80 * 24 + i] = ' ' | 0x7000;
    print_string(" ClockOS Edit | ESC to Save | Arrows to Move", (uint16_t*)(VGA_TEXT + 80 * 24 * 2));
    
    int x = 0, y = 0;
    for (uint32_t i = 0; i < edit_len; i++) {
        if (edit_buffer[i] == '\n') {
            x = 0; y++;
            if (y >= 24) break;
        } else {
            if (y < 24) {
                vid[y * 80 + x] = edit_buffer[i] | color;
            }
            x++;
            if (x >= 80) { x = 0; y++; }
        }
    }
    
    int cx = 0, cy = 0;
    for (uint32_t i = 0; i < edit_cursor; i++) {
        if (edit_buffer[i] == '\n') { cx = 0; cy++; }
        else { cx++; if (cx >= 80) { cx = 0; cy++; } }
    }
    cursor_x = cx > 79 ? 79 : cx;
    cursor_y = cy > 23 ? 23 : cy;
    update_hardware_cursor();
}
/*
static void ata_delay_400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(0x3F6);
}

static int ata_identify(int slave) {
    outb(0x1F6, 0xA0 | (slave ? 0x10 : 0x00));
    ata_delay_400ns();
    if (inb(0x1F7) == 0xFF) return 0;   // floating bus：没有传统 IDE

    outb(0x1F6, 0xA0 | (slave ? 0x10 : 0x00));
    outb(0x1F2, 0);
    outb(0x1F3, 0);
    outb(0x1F4, 0);
    outb(0x1F5, 0);
    outb(0x1F7, 0xEC);                   // IDENTIFY
    ata_delay_400ns();
    if (inb(0x1F7) == 0) return 0;       // 无设备

    for (int i = 0; i < 200000; i++) {
        uint8_t st = inb(0x1F7);
        if (st == 0) return 0;
        if (!(st & 0x80)) {
            uint8_t mid = inb(0x1F4);
            uint8_t hi  = inb(0x1F5);
            if (mid != 0 || hi != 0) return 0;  // 不是 ATA
            if (st & 0x01) return 0;            // ERR
            return 1;
        }
    }
    return 0;
}

static void ata_init(void) {
    g_ata_ready = 0;
    g_ata_slave = 0;
    if (inb(0x1F7) == 0xFF) return;

    int want = -1;
    if (g_boot_info->magic == BOOT_INFO_MAGIC) {
        if (g_boot_info->drive_number == 0x80) want = 0;
        else if (g_boot_info->drive_number == 0x81) want = 1;
    }

    if (want >= 0 && ata_identify(want)) {
        g_ata_slave = want;
        g_ata_ready = 1;
        return;
    }
    if (ata_identify(0)) { g_ata_slave = 0; g_ata_ready = 1; return; }
    if (ata_identify(1)) { g_ata_slave = 1; g_ata_ready = 1; return; }
}

void ata_wait_bsy() {
    while (inb(0x1F7) & 0x80);
}

void ata_wait_drq() {
    while (!(inb(0x1F7) & 0x08));
}

int ata_wait_bsy_timeout() {
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inb(0x1F7) & 0x80)) return 1;
    }
    return 0;
}

int ata_wait_drq_timeout() {
    for (uint32_t i = 0; i < 100000; i++) {
        if (inb(0x1F7) & 0x08) return 1;
    }
    return 0;
}

// 底层硬件操作封装
static void ata_io_wait(uint32_t timeout) {
    for (volatile uint32_t i = 0; i < timeout; i++);
}

// 等待设备就绪 (BSY=0, DRDY=1)
static int ata_poll(uint32_t timeout_ticks) {
    while (timeout_ticks--) {
        uint8_t status = inb(0x1F7);
        if (status & 0x01) return DEV_ERR_IO; // ERR 位
        if (!(status & 0x80) && (status & 0x40)) return DEV_OK; // BSY=0, RDY=1
        ata_io_wait(100);
    }
    return DEV_ERR_TIMEOUT;
}

void ata_soft_reset(void) {
    outb(0x3F6, 0x04); // SRST bit
    ata_io_wait(100000);
    outb(0x3F6, 0x00);
    ata_io_wait(100000);
}

// 标准块读取 API 实现
int block_read(block_request_t *req) {
    if (!g_ata_ready) return DEV_ERR_NOTRDY;
    
    int ret = ata_poll(100000);
    if (ret != DEV_OK) return ret;

    outb(0x1F6, 0xE0 | (g_ata_slave << 4) | ((req->lba >> 24) & 0x0F));
    outb(0x1F2, (uint8_t)req->count);
    outb(0x1F3, (uint8_t)req->lba);
    outb(0x1F4, (uint8_t)(req->lba >> 8));
    outb(0x1F5, (uint8_t)(req->lba >> 16));
    outb(0x1F7, 0x20); // READ SECTORS

    uint16_t *ptr = (uint16_t*)req->buffer;
    for (int s = 0; s < req->count; s++) {
        ret = ata_poll(100000);
        if (ret != DEV_OK) return ret;
        
        // 等待 DRQ 置位
        while (!(inb(0x1F7) & 0x08)) {
            if (inb(0x1F7) & 0x01) return DEV_ERR_IO;
        }

        for (int i = 0; i < 256; i++) {
            *ptr++ = inw(0x1F0);
        }
    }
    return DEV_OK;
}

// 标准块写入 API
int block_write(block_request_t *req) {
    if (!g_ata_ready) return DEV_ERR_NOTRDY;
    
    // 1. 等待设备不忙
    uint32_t timeout = 1000000;
    while (inb(0x1F7) & 0x80) {
        if (--timeout == 0) return DEV_ERR_TIMEOUT; 
    }

    // 2. 发送指令
    outb(0x1F6, 0xE0 | (g_ata_slave << 4) | ((req->lba >> 24) & 0x0F));
    outb(0x1F2, (uint8_t)req->count);
    outb(0x1F3, (uint8_t)req->lba);
    outb(0x1F4, (uint8_t)(req->lba >> 8));
    outb(0x1F5, (uint8_t)(req->lba >> 16));
    outb(0x1F7, 0x30); // WRITE SECTORS

    const uint16_t *ptr = (const uint16_t*)req->buffer;
    for (int s = 0; s < req->count; s++) {
        // 3. 等待 DRQ 置位 (加入超时)
        timeout = 1000000;
        while (1) {
            uint8_t status = inb(0x1F7);
            if (status & 0x01) return DEV_ERR_IO; // 报错了
            if (!(status & 0x80) && (status & 0x08)) break; // 准备就绪，跳出
            if (--timeout == 0) return DEV_ERR_TIMEOUT; // 超时！拒绝死等
        }

        // 4. 写入 512 字节数据
        for (int i = 0; i < 256; i++) {
            outw(0x1F0, *ptr++);
        }
    }

    // 5. FLUSH CACHE
    outb(0x1F7, 0xE7);
    timeout = 1000000;
    while (inb(0x1F7) & 0x80) {
        if (--timeout == 0) return DEV_ERR_TIMEOUT;
    }
    
    return DEV_OK;
}
*/

void fs_sync_to_disk(void) {
    static uint8_t sb_buf[512];
    static uint8_t inode_buf[FS_INODE_SECS * 512];
    static uint8_t bitmap_buf[FS_BITMAP_SECS * 512];
    static uint8_t data_buf[512];
    static uint32_t indirect_buf[128];

    /* ===== 阶段 A：规划 inode ===== */
    print_line("SYNC: 1. Start...");
    for (int i = 0; i < (int)sizeof(inode_buf); i++) inode_buf[i] = 0;

    uint32_t next_block = 0;

    for (int i = 0; i < FS_MAX_INODES; i++) {
        struct inode *ino = (struct inode *)(inode_buf + i * 128);

        if (!file_system[i].used) {
            ino->type = 0;
            continue;
        }

        ino->type = 1;
        ino->mode = 0;
        ino->size = file_system[i].size;
        
        int n = 0;
        while (n < 31 && file_system[i].name[n]) {
            ino->name[n] = file_system[i].name[n];
            n++;
        }
        ino->name[n] = 0;
        for (int k = 0; k < 12; k++) ino->blocks[k] = 0;
        ino->indirect_block = 0;

        uint32_t sectors = (file_system[i].size + 511) / 512;
        // 最大支持 12 个直接块 + 128 个间接块 = 140 个扇区
        if (sectors > 140) {
            ino->type = 0; 
            continue;
        }

        if (sectors <= 12) {
            // 小文件：只分配直接块
            for (uint32_t s = 0; s < sectors; s++) {
                ino->blocks[s] = next_block + s;
            }
            next_block += sectors;
        } else {
            // 大文件：分配 12 个直接块
            for (uint32_t s = 0; s < 12; s++) {
                ino->blocks[s] = next_block + s;
            }
            // 分配 1 个间接块本身
            ino->indirect_block = next_block + 12;
            // 分配剩下的数据块
            next_block += 12 + 1 + (sectors - 12);
        }
    }

    /* ===== 阶段 B：写数据区 ===== */
    print_line("SYNC: 2. Writing Data...");
    uint32_t written_block = 0;

    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (!file_system[i].used) continue;
        uint32_t size = file_system[i].size;
        if (size == 0) continue;
        uint32_t sectors = (size + 511) / 512;
        
        uint32_t data_addr = (uint32_t)file_system[i].data;
        if (data_addr < 0x200000 || data_addr >= 0x400000) continue;

        if (sectors <= 12) {
            for (uint32_t s = 0; s < sectors; s++) {
                for (int j = 0; j < 512; j++) {
                    uint32_t off = s * 512 + j;
                    data_buf[j] = (off < size) ? file_system[i].data[off] : 0;
                }
                ata_write_sector(FS_DATA_LBA + written_block, data_buf);
                written_block++;
            }
        } else {
            // 1. 写前 12 个直接块
            for (uint32_t s = 0; s < 12; s++) {
                for (int j = 0; j < 512; j++) {
                    uint32_t off = s * 512 + j;
                    data_buf[j] = (off < size) ? file_system[i].data[off] : 0;
                }
                ata_write_sector(FS_DATA_LBA + written_block, data_buf);
                written_block++;
            }

            // 2. 构造并写入间接块
            for (int k = 0; k < 128; k++) indirect_buf[k] = 0;
            for (uint32_t s = 12; s < sectors; s++) {
                // 记录后续数据块的相对块号
                indirect_buf[s - 12] = (written_block + 1) + (s - 12);
            }
            ata_write_sector(FS_DATA_LBA + written_block, (uint8_t*)indirect_buf);
            written_block++;

            // 3. 写剩下的数据块
            for (uint32_t s = 12; s < sectors; s++) {
                for (int j = 0; j < 512; j++) {
                    uint32_t off = s * 512 + j;
                    data_buf[j] = (off < size) ? file_system[i].data[off] : 0;
                }
                ata_write_sector(FS_DATA_LBA + written_block, data_buf);
                written_block++;
            }
        }
    }

    /* 写 Inode 表、位图、超级块  */
    print_line("SYNC: 3. Writing Inodes...");
    for (int s = 0; s < FS_INODE_SECS; s++) {
        ata_write_sector(FS_INODE_LBA + s, inode_buf + s * 512);
    }

    print_line("SYNC: 4. Writing Bitmap...");
    for (int i = 0; i < (int)sizeof(bitmap_buf); i++) bitmap_buf[i] = 0;
    for (uint32_t b = 0; b < next_block; b++) {
        bitmap_buf[b / 8] |= (1u << (b % 8));
    }
    for (int s = 0; s < FS_BITMAP_SECS; s++) {
        ata_write_sector(FS_BITMAP_LBA + s, bitmap_buf + s * 512);
    }

    print_line("SYNC: 5. Writing Superblock...");
    for (int i = 0; i < 512; i++) sb_buf[i] = 0;
    struct superblock *sb = (struct superblock *)sb_buf;
    sb_buf[0] = FS_MAGIC_B0;
    sb_buf[1] = FS_MAGIC_B1;
    sb_buf[2] = FS_MAGIC_B2;
    sb_buf[3] = FS_MAGIC_B3;
    sb->block_size = 512; sb->total_blocks = 2880;
    sb->inode_count = FS_MAX_INODES;
    sb->inode_start_lba = FS_INODE_LBA; sb->inode_blocks = FS_INODE_SECS;
    sb->bitmap_start_lba = FS_BITMAP_LBA; sb->bitmap_blocks = FS_BITMAP_SECS;
    sb->data_start_lba = FS_DATA_LBA;
    ata_write_sector(FS_SUPER_LBA, sb_buf);

    print_line("SYNC: 6. Flushing...");
    ata_flush_cache();
    print_line("SYNC: Done!");
}

// 返回值：0 = 挂载成功；1 = 无有效文件系统；2 = 无 ATA 设备/读失败
int fs_mount(void) {
    static uint8_t sb_buf[512];
    static uint8_t inode_buf[FS_INODE_SECS * 512];
    static uint8_t data_buf[512];

    if (!ata_init()) return 2;

    // 直接去 145 读
    int res = ata_read_sector(FS_SUPER_LBA, sb_buf);
    if (res != ATA_OK) return 2;

    struct superblock *sb = (struct superblock *)sb_buf;

    // 只校验最核心的魔法数字，其他的全放行！
    if (sb_buf[0] != FS_MAGIC_B0 || sb_buf[1] != FS_MAGIC_B1 || 
        sb_buf[2] != FS_MAGIC_B2 || sb_buf[3] != FS_MAGIC_B3) {
        // 这里保留我们上次加的开天眼打印
        print_string("Read Magic: 0x", get_cursor_pos());
        cursor_x += 15;
        for(int i=0; i<4; i++) {
            uint8_t b = sb_buf[i];
            uint16_t* vid = get_cursor_pos();
            *vid = (b < 16 ? '0' : 'A' - 10 + b) | 0x0F00; cursor_x++;
            *vid = ((b & 0x0F) < 10 ? '0' + (b & 0x0F) : 'A' - 10 + (b & 0x0F)) | 0x0F00; cursor_x++;
        }
        print_line("");
        return 1;
    }
    
    // 为了防止后面的数据错位读错，我们直接硬编码 LBA 位置！
    // 反正你的 HxD 证明它们都在正确的位置。
    fs_init();

    // 读 inode 表
    for (int s = 0; s < FS_INODE_SECS; s++) {
        res = ata_read_sector(FS_INODE_LBA + s, inode_buf + s * 512);
        if (res != ATA_OK) return 2;
    }

    // 解析 inode 表
    for (int i = 0; i < FS_MAX_INODES; i++) {
        struct inode *ino = (struct inode *)(inode_buf + i * 128);

        if (ino->type != 1 || ino->size > 12 * 512) {
            file_system[i].used = 0;
            continue;
        }

        file_system[i].used = 1;
        file_system[i].size = ino->size;
        file_system[i].create_tick = 0;

        int n = 0;
        while (n < 31 && ino->name[n]) {
            file_system[i].name[n] = ino->name[n];
            n++;
        }
        file_system[i].name[n] = 0;
        file_system[i].data = 0; 
    }

    static uint32_t indirect_buf[128];
    for (int i = 0; i < FS_MAX_INODES; i++) {
        if (!file_system[i].used || file_system[i].size == 0) continue;

        struct inode *ino = (struct inode *)(inode_buf + i * 128);
        uint32_t size = ino->size;
        uint32_t sectors = (size + 511) / 512;

        if (sectors > 140) {
            file_system[i].used = 0;
            file_system[i].size = 0;
            continue;
        }

        uint8_t *buf = (uint8_t *)kalloc(size + 1);
        if (!buf) {
            file_system[i].data = 0;
            continue;
        }

        int read_fail = 0;

        // 1. 读前 12 个直接块
        uint32_t direct_sectors = (sectors <= 12) ? sectors : 12;
        for (uint32_t s = 0; s < direct_sectors; s++) {
            uint32_t lba = FS_DATA_LBA + ino->blocks[s];
            if (ata_read_sector(lba, data_buf) != ATA_OK) { read_fail = 1; break; }
            for (int j = 0; j < 512; j++) {
                uint32_t off = s * 512 + j;
                if (off < size) buf[off] = data_buf[j];
            }
        }

        // 2. 如果需要，读取间接块及其指向的数据块
        if (!read_fail && sectors > 12) {
            // 读取间接块本身
            uint32_t ind_lba = FS_DATA_LBA + ino->indirect_block;
            if (ata_read_sector(ind_lba, (uint8_t*)indirect_buf) != ATA_OK) {
                read_fail = 1;
            } else {
                // 读取间接块指向的数据
                for (uint32_t s = 12; s < sectors; s++) {
                    uint32_t block_idx = s - 12;
                    if (block_idx >= 128) break;
                    
                    uint32_t lba = FS_DATA_LBA + indirect_buf[block_idx];
                    if (ata_read_sector(lba, data_buf) != ATA_OK) { read_fail = 1; break; }
                    for (int j = 0; j < 512; j++) {
                        uint32_t off = s * 512 + j;
                        if (off < size) buf[off] = data_buf[j];
                    }
                }
            }
        }

        if (read_fail) {
            kfree(buf, size + 1);
            file_system[i].data = 0;
            file_system[i].size = 0;
        } else {
            buf[size] = 0;
            file_system[i].data = buf;
        }
    }
    return 0;
}

/*
void fs_load_from_disk(void) {
    uint8_t buffer[512];
    for (int i = 0; i < 512; i++) buffer[i] = 0;

    // 1. 初始化 ATA 控制器
    if (!ata_init()) {
        print_line("FS: no ATA disk, RAM-only mode.");
        fs_init();
        return;
    }

    // 2. 读取元数据
    int res = ata_read_sector(FS_META_LBA, buffer);
    if (res != ATA_OK) {
        print_line("FS: Read failed, RAM-only mode.");
        print_line(ata_get_error_string(res));
        fs_init();
        return;
    }

    // 3. 检查魔法数字
    if (buffer[0] != 'C' || buffer[1] != 'L' ||
        buffer[2] != 'O' || buffer[3] != 'C') {
        print_line("FS: no valid FS, initializing...");
        fs_init();
        fs_sync_to_disk(); // 初始化写盘
        return;
    }

    // 4. 解析文件元数据
    for (int i = 0; i < MAX_FILES; i++) {
        uint32_t base = 8 + i * 16;
        file_system[i].used = buffer[base + 0];
        file_system[i].size = buffer[base + 1]
                            | (buffer[base + 2] << 8)
                            | (buffer[base + 3] << 16);
        for (int j = 0; j < 12; j++) {
            file_system[i].name[j] = buffer[base + 4 + j];
        }
        file_system[i].name[12] = 0;
        file_system[i].data = 0; 

        if (file_system[i].used != 1) {
            file_system[i].used = 0;
            file_system[i].size = 0;
            file_system[i].name[0] = 0;
        }
    }

    // 5. 读取文件真实数据
    uint32_t current_lba = FS_DATA_LBA;
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_system[i].used && file_system[i].size > 0) {
            uint32_t size = file_system[i].size;
            uint32_t sectors = (size + 511) / 512;
            
            file_system[i].data = (uint8_t*)kalloc(size + 1);
            if (file_system[i].data) {
                for (uint32_t s = 0; s < sectors; s++) {
                    uint8_t buf[512];
                    if (ata_read_sector(current_lba + s, buf) == ATA_OK) {
                        for (int j = 0; j < 512; j++) {
                            uint32_t off = s * 512 + j;
                            if (off < size) file_system[i].data[off] = buf[j];
                        }
                    }
                }
                file_system[i].data[size] = 0;
            }
            current_lba += sectors;
        }
    }
}
*/

void fs_format(void) {
    fs_init();
    fs_sync_to_disk();
}

void clear_screen(void) {
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    for (int i = 0; i < 2000; i++)
        vid[i] = 0x0720;
    cursor_x = cursor_y = 0;
    update_hardware_cursor();
}

void print_string(const char *str, uint16_t *vidptr) {
    uint16_t *vid = vidptr ? vidptr : get_cursor_pos();
    while (*str) {
        *vid++ = (uint16_t)((uint8_t)*str++) | (text_color << 8);
    }
}

void print_line(const char *str) {
    print_string(str, get_cursor_pos());
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= 25) {
        cursor_y = 24;
        scroll_screen();
    }
    update_hardware_cursor();
}

void set_vga_text(void) {
    uint8_t text_regs[] = {
        0x63, 0x03, 0x01, 0x0F, 0x00, 0x0E,
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
        0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
        0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x41, 0x00, 0x0F, 0x00, 0x00
    };
    uint8_t *regs = text_regs;
    outb(0x3C2, *regs++);
    for (int i = 4; i >= 0; i--) { outb(0x3C4, i); outb(0x3C5, *regs++); }
    outb(0x3D4, 0x03); outb(0x3D5, inb(0x3D5) | 0x80);
    for (int i = 24; i >= 0; i--) { outb(0x3D4, i); outb(0x3D5, *regs++); }
    for (int i = 8; i >= 0; i--) { outb(0x3CE, i); outb(0x3CF, *regs++); }
    inb(0x3DA);
    for (int i = 20; i >= 0; i--) { outb(0x3C0, i); outb(0x3C0, *regs++); }
    outb(0x3C0, 0x20);
    display_mode = 0;
}

void set_vga_gfx(void) {
    const uint8_t *regs = vga_gfx_regs;
    outb(0x3C2, *regs++);
    // Sequencer
    for (int i = 4; i >= 0; i--) {
        outb(0x3C4, (uint8_t)i);
        outb(0x3C5, *regs++);
    }
    outb(0x3D4, 0x03);
    outb(0x3D5, inb(0x3D5) | 0x80);
    for (int i = 24; i >= 0; i--) {
        outb(0x3D4, (uint8_t)i);
        outb(0x3D5, *regs++);
    }
    for (int i = 8; i >= 0; i--) {
        outb(0x3CE, (uint8_t)i);
        outb(0x3CF, *regs++);
    }
    inb(0x3DA);
    for (int i = 20; i >= 0; i--) {
        outb(0x3C0, (uint8_t)i);
        outb(0x3C0, *regs++);
    }
    outb(0x3C0, 0x20);
}

void scroll_screen(void) {
    uint16_t *vid = (uint16_t *)VGA_TEXT;
    for (int i = 0; i < 24 * 80; i++)
        vid[i] = vid[i + 80];
    for (int i = 24 * 80; i < 25 * 80; i++)
        vid[i] = 0x0720;
}

void init_pic(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}

void set_idt_gate(int n, uint32_t handler, uint16_t selector, uint8_t flags) {
    idt[n].base_low  = handler & 0xFFFF;
    idt[n].base_high = (handler >> 16) & 0xFFFF;
    idt[n].selector  = selector;
    idt[n].zero      = 0;
    idt[n].flags     = flags;
}

void init_idt(void) {
    for (int i = 0; i < 256; i++) {
        idt[i].base_low = 0;
        idt[i].base_high = 0;
        idt[i].selector = 0;
        idt[i].zero = 0;
        idt[i].flags = 0;
    }
    for (int i = 0; i < 32; i++) {
        if (i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30) {
            set_idt_gate(i, (uint32_t)exception_err_handler, 0x08, 0x8E);
        } else {
            set_idt_gate(i, (uint32_t)exception_handler, 0x08, 0x8E);
        }
    }
    set_idt_gate(8, (uint32_t)double_fault_handler, 0x08, 0x8E);
    set_idt_gate(33, (uint32_t)isr_keyboard, 0x08, 0x8E);

    idt_descriptor.limit = sizeof(idt) - 1;
    idt_descriptor.base  = (uint32_t)&idt;
    lidt((void *)idt_descriptor.base, idt_descriptor.limit);
}

void update_hardware_cursor(void) {
    if (display_mode == 1) return;
    uint16_t pos = cursor_y * 80 + cursor_x;
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
}

uint16_t *get_cursor_pos(void) {
    uint32_t y = cursor_y > 24 ? 24 : cursor_y;
    uint32_t x = cursor_x > 79 ? 79 : cursor_x;
    return (uint16_t *)(VGA_TEXT + (y * 80 + x) * 2);
}

void inc_cursor(void) {
    cursor_x++;
    if (cursor_x >= 80) {
        cursor_x = 0;
        cursor_y++;
    }
    if (cursor_y >= 25) {
        scroll_screen();
        cursor_y = 24;
    }
}

void dec_cursor(void) {
    if (cursor_x == 0) {
        if (cursor_y > 0) {
            cursor_x = 79;
            cursor_y--;
        }
    } else {
        cursor_x--;
    }
}

void draw_pixel(uint16_t x, uint16_t y, uint8_t color) {
    uint8_t *gfx = (uint8_t *)VGA_GFX;
    gfx[y * 320 + x] = color;
}

void kmain(void) {
    cli();
    clear_screen(); // 此时 cursor_x=0, cursor_y=0

    // 手动控制光标并打印
    print_string(msg_booting, get_cursor_pos());
    cursor_y = 1; update_hardware_cursor(); // 光标移到下一行，解决闪烁卡在B的问题

    static struct bcb_t bcb;
    if (ata_init() && ata_read_sector(BCB_LBA, (uint8_t*)&bcb) == ATA_OK && bcb.magic == 0x42434200) {
        if (bcb.boot_success == 0) {
            bcb.boot_success = 1; // 标记成功，防止回滚
            ata_write_sector(BCB_LBA, (uint8_t*)&bcb);
            ata_flush_cache();
        }
    }
    
    print_string(msg_step1, get_cursor_pos());
    cursor_y = 2; update_hardware_cursor();
    
    print_string(msg_step2, get_cursor_pos());
    cursor_y = 3; update_hardware_cursor();

    //PIC初始化
    print_string("Initializing PIC...", get_cursor_pos());
    cursor_y = 4; update_hardware_cursor();
    init_pic();

    //IDT初始化
    print_string("Initializing IDT...", get_cursor_pos());
    cursor_y = 5; update_hardware_cursor();
    init_idt();

    // 磁盘加载
    print_string("Reading file system from disk...", get_cursor_pos());
    cursor_y++; update_hardware_cursor(); // 正常换行
    
    fs_init(); // 挂载前清空内存
    int fs_status = fs_mount();
    
    if (fs_status == 0) {
        print_line("FS mounted.");
    } else if (fs_status == 2) {
        print_line("No ATA disk. RAM-only mode.");
        fs_init();
    } else {
        /* fs_status == 1：无有效文件系统 */
        print_string("No valid FS. Format disk? (Y/N): ", get_cursor_pos());
        
        // 等待按键
        uint8_t answer = 0;
        while (1) {
            if (inb(0x64) & 1) {
                uint8_t sc = inb(0x60);
                if (sc == 0x15) { answer = 'Y'; break; }   /* Y 键 */
                if (sc == 0x31) { answer = 'N'; break; }   /* N 键 */
            }
        }
        
        if (answer == 'Y') {
            print_line("Y"); // 让屏幕显示你按了 Y
            print_line("Formatting disk, please wait...");
            fs_format();     // 这里面会干净地打印 SYNC 1~6
            print_line("Format complete. Welcome!");
        } else {
            print_line("N");
            print_line("RAM-only mode.");
            fs_init(); 
        }
    }

    // 加载完成，一气呵成地打印后面的信息，绝不再强设 cursor_y
    print_line(""); // 空一行
    print_line(msg_step1);
    print_line(msg_step2);
    print_line(msg_step3);
    print_line(msg_step4);
    print_string(msg_prompt, get_cursor_pos());
    
    // 设置最终的光标位置在提示符后面
    cursor_x += 10; 
    update_hardware_cursor();

    last_scancode = 0;
    feedback_eoi = ' ';
    buf_head = buf_tail = buf_len = 0;
    shift_state = 0;

    sti();

    while (1) {
        hlt();
    }
}