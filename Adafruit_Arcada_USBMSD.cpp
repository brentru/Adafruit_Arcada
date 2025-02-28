#include <Adafruit_Arcada.h>

//#define ARCADA_MSD_DEBUG

static uint32_t last_access_ms;

#if defined(USE_TINYUSB)
static Adafruit_USBD_MSC usb_msc;

#if defined(ARCADA_USE_QSPI_FS)
  extern Adafruit_QSPI_Flash arcada_qspi_flash;

  int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize);
  int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize);
  void msc_flush_cb (void);
  void flash_cache_read (uint8_t* dst, uint32_t addr, uint32_t count);
  uint32_t flash_cache_write (uint32_t dst, void const * src, uint32_t len);
  void flash_cache_flush (void);

  #define FLASH_CACHE_SIZE          4096        // must be a erasable page size
  #define FLASH_CACHE_INVALID_ADDR  0xffffffff

  uint32_t cache_addr = FLASH_CACHE_INVALID_ADDR;
  uint8_t  cache_buf[FLASH_CACHE_SIZE];
#elif defined(ARCADA_USE_SD_FS)
  extern SdFat FileSys;

  int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize);
  int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize);
  void msc_flush_cb (void);
#endif // SD or QSPI
#endif // TinyUSB

/**************************************************************************/
/*!
    @brief  Make the raw filesystem of the Arcada board available over USB
    @return True on success, false on failure
*/
/**************************************************************************/
bool Adafruit_Arcada::filesysBeginMSD(void) {
#if defined(USE_TINYUSB) && defined(ARCADA_USE_QSPI_FS)
  if (!arcada_qspi_flash.begin()) {
    return false;
  }

  // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID("Adafruit", "SPI Flash", "1.0");

  // Set callback
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  // Set disk size, block size should be 512 regardless of spi flash page size
  usb_msc.setCapacity(arcada_qspi_flash.pageSize()*arcada_qspi_flash.numPages()/512, 512);

  // MSC is ready for read/write
  usb_msc.setUnitReady(true);  
  usb_msc.begin();
  return true;

#elif defined(USE_TINYUSB) && defined(ARCADA_USE_SD_FS)
  if (!filesysBegin()) {
    return false;
  }

 // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID("Adafruit", "SD Card", "1.0");

  // Set callback
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);

  uint32_t block_count = FileSys.vol()->blocksPerCluster()*FileSys.vol()->clusterCount();
  Serial.print("Volume size (MB):  ");
  Serial.println((block_count/2) / 1024);

  // Set disk size, SD block size is always 512
  usb_msc.setCapacity(block_count, 512);

  // MSC is ready for read/write
  usb_msc.setUnitReady(true);  
  usb_msc.begin();
  return true;
#else
  return false;
#endif
  return false;
}

/**************************************************************************/
/*!
    @brief  Hints whether we're doing a bunch of USB stuff recently
    @param  timeout The timeperiod to look at, defaults to 100ms
    @return True if some USB stuff happened in last timeout # millis
*/
/**************************************************************************/
bool Adafruit_Arcada::recentUSB(uint32_t timeout) {

#if defined(USE_TINYUSB)
  uint32_t curr_time = millis();
  if (last_access_ms > curr_time) {  // oi, rollover
    return false;
  }
  if ((last_access_ms + timeout) >= curr_time) {
    return true;  // indeed!
  }
#endif
  return false;
}

#if  defined(USE_TINYUSB) && defined(ARCADA_USE_QSPI_FS)

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and 
// return number of copied bytes (must be multiple of block size) 
int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize)
{
  const uint32_t addr = lba*512;
  flash_cache_read((uint8_t*) buffer, addr, bufsize);
#ifdef ARCADA_MSD_DEBUG
  Serial.printf("Read block %08x\n", lba);
#endif
  last_access_ms = millis();
  return bufsize;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and 
// return number of written bytes (must be multiple of block size)
int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize)
{
#ifdef ARCADA_MSD_DEBUG
  Serial.printf("Write block %08x\n", lba);
#endif
  // need to erase & caching write back
  const uint32_t addr = lba*512;
  flash_cache_write(addr, buffer, bufsize);
  last_access_ms = millis();
  return bufsize;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void msc_flush_cb (void)
{
#ifdef ARCADA_MSD_DEBUG
  Serial.printf("Flush block\n");
#endif
  last_access_ms = millis();
  flash_cache_flush();
}

//--------------------------------------------------------------------+
// Flash Caching
//--------------------------------------------------------------------+
static inline uint32_t page_addr_of (uint32_t addr)
{
  return addr & ~(FLASH_CACHE_SIZE - 1);
}

static inline uint32_t page_offset_of (uint32_t addr)
{
  return addr & (FLASH_CACHE_SIZE - 1);
}

void flash_cache_flush (void)
{
  if ( cache_addr == FLASH_CACHE_INVALID_ADDR ) return;

  // indicator
  digitalWrite(LED_BUILTIN, HIGH);

  arcada_qspi_flash.eraseSector(cache_addr/FLASH_CACHE_SIZE);
  arcada_qspi_flash.writeBuffer(cache_addr, cache_buf, FLASH_CACHE_SIZE);

  digitalWrite(LED_BUILTIN, LOW);

  cache_addr = FLASH_CACHE_INVALID_ADDR;
}

uint32_t flash_cache_write (uint32_t dst, void const * src, uint32_t len)
{
  uint8_t const * src8 = (uint8_t const *) src;
  uint32_t remain = len;

  // Program up to page boundary each loop
  while ( remain )
  {
    uint32_t const page_addr = page_addr_of(dst);
    uint32_t const offset = page_offset_of(dst);

    uint32_t wr_bytes = FLASH_CACHE_SIZE - offset;
    wr_bytes = min(remain, wr_bytes);

    // Page changes, flush old and update new cache
    if ( page_addr != cache_addr )
    {
      flash_cache_flush();
      cache_addr = page_addr;

      // read a whole page from flash
      arcada_qspi_flash.readBuffer(page_addr, cache_buf, FLASH_CACHE_SIZE);
    }

    memcpy(cache_buf + offset, src8, wr_bytes);

    // adjust for next run
    src8 += wr_bytes;
    remain -= wr_bytes;
    dst += wr_bytes;
  }

  return len - remain;
}

void flash_cache_read (uint8_t* dst, uint32_t addr, uint32_t count)
{
  // overwrite with cache value if available
  if ( (cache_addr != FLASH_CACHE_INVALID_ADDR) &&
       !(addr < cache_addr && addr + count <= cache_addr) &&
       !(addr >= cache_addr + FLASH_CACHE_SIZE) )
  {
    int dst_off = cache_addr - addr;
    int src_off = 0;

    if ( dst_off < 0 )
    {
      src_off = -dst_off;
      dst_off = 0;
    }

    int cache_bytes = min(FLASH_CACHE_SIZE-src_off, count - dst_off);

    // start to cached
    if ( dst_off ) arcada_qspi_flash.readBuffer(addr, dst, dst_off);

    // cached
    memcpy(dst + dst_off, cache_buf + src_off, cache_bytes);

    // cached to end
    int copied = dst_off + cache_bytes;
    if ( copied < count ) arcada_qspi_flash.readBuffer(addr + copied, dst + copied, count - copied);
  }
  else
  {
    arcada_qspi_flash.readBuffer(addr, dst, count);
  }
}

#elif defined(USE_TINYUSB) && defined(ARCADA_USE_SD_FS)
int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize)
{
  (void) bufsize;
  last_access_ms = millis();
  return FileSys.card()->readBlock(lba, (uint8_t*) buffer) ? 512 : -1;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and 
// return number of written bytes (must be multiple of block size)
int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize)
{
  last_access_ms = millis();
  return FileSys.card()->writeBlock(lba, buffer) ? 512 : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void msc_flush_cb (void)
{
  last_access_ms = millis();
  // nothing to do
}

#endif // QSPI or SD

