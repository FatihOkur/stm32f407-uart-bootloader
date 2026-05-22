#ifndef IMAGE_STORE_H
#define IMAGE_STORE_H
#include "protocol.h"
/* Verify the actual flash bytes, then commit a record in pre-erased metadata. */
ProtocolStatus Image_Commit(uint32_t size,uint32_t crc,const uint16_t version[3]);
/* Re-read metadata and the complete flash image every call. No RAM validity flag. */
uint8_t Image_StoredValid(void);
/* Boot decision: USER overrides valid images. No vector-only fallback.
 * Outputs are zero on rejection; call before any UART update processing. */
uint8_t Image_GetBootVectors(uint8_t force_bootloader,uint32_t *msp,uint32_t *reset);
#endif
