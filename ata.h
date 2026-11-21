#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_OK          0
#define ATA_ERR_TIMEOUT 1
#define ATA_ERR_IO      2
#define ATA_ERR_NOTRDY  3
#define ATA_ERR_INVAL   4 
int ata_init(void);
int ata_flush_cache(void);

int ata_read_sector(uint32_t lba, uint8_t* buffer);
int ata_write_sector(uint32_t lba, const uint8_t* buffer);

const char* ata_get_error_string(int err_code);

extern int g_ata_slave;

#endif