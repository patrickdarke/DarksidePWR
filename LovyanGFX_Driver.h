#define LGFX_USE_V1                      // Enable LovyanGFX version 1
#include <LovyanGFX.hpp>                 // Include LovyanGFX main header
#include <driver/i2c.h>                  // Include ESP-IDF I2C driver definitions

class LGFX : public lgfx::LGFX_Device     // Define LGFX class inheriting from LGFX_Device
{
    lgfx::Panel_ILI9488     _panel_instance;   // ILI9488 display panel instance
    lgfx::Bus_SPI           _bus_instance;     // SPI bus instance
    lgfx::Touch_GT911       _touch_instance;   // GT911 touch controller instance

  public:
    LGFX(void) {                           // LGFX class constructor
      {
        auto cfg = _bus_instance.config(); // Get SPI bus configuration structure

        // SPI bus configuration
        cfg.spi_host = SPI2_HOST;          // Select SPI host (SPI2_HOST or SPI3_HOST for ESP32-S2/S3/C3)
        // With newer ESP-IDF versions, VSPI_HOST and HSPI_HOST are deprecated.
        // If errors occur, use SPI2_HOST or SPI3_HOST instead.
        cfg.spi_mode = 0;                  // Set SPI communication mode (0~3)
        cfg.freq_write = 40000000;         // SPI clock frequency for write operations (max 80MHz)
        cfg.freq_read = 16000000;          // SPI clock frequency for read operations
        cfg.spi_3wire = false;             // Set true if receiving data via MOSI pin
        cfg.use_lock = true;               // Enable transaction lock for SPI bus
        cfg.dma_channel = SPI_DMA_CH_AUTO; // Automatically select DMA channel
        // New ESP-IDF versions recommend using SPI_DMA_CH_AUTO for DMA channel selection.
        cfg.pin_sclk = 42;                 // SPI SCLK pin number
        cfg.pin_mosi = 39;                 // SPI MOSI pin number
        cfg.pin_miso = -1;                 // SPI MISO pin number (-1 = disabled)
        cfg.pin_dc = 41;                   // SPI DC (Data/Command) pin number (-1 = disabled)

        _bus_instance.config(cfg);              // Apply SPI bus configuration
        _panel_instance.setBus(&_bus_instance); // Attach SPI bus to the display panel
      }

      { // Configure display panel control settings
        auto cfg = _panel_instance.config(); // Get panel configuration structure

        cfg.pin_cs = 40;                 // Chip Select (CS) pin number (-1 = disabled)
        cfg.pin_rst = 2;                 // Reset (RST) pin number (-1 = disabled)
        cfg.pin_busy = -1;               // Busy pin number (-1 = disabled)

        // The following default values are set for each panel.
        // BUSY pin is disabled by default (-1). If unsure, try commenting settings individually.

        cfg.memory_width = 320;           // Maximum width supported by the panel driver IC
        cfg.memory_height = 480;          // Maximum height supported by the panel driver IC
        cfg.panel_width = 320;            // Actual visible display width
        cfg.panel_height = 480;           // Actual visible display height
        cfg.offset_x = 0;                 // Horizontal offset of the display
        cfg.offset_y = 0;                 // Vertical offset of the display
        cfg.offset_rotation = 3;          // Rotation offset value (0~7, 4~7 are inverted)
        cfg.dummy_read_pixel = 8;         // Dummy bits before reading pixel data
        cfg.dummy_read_bits = 1;          // Dummy bits before reading non-pixel data
        cfg.readable = false;             // Set true if panel supports read operations
        cfg.invert = true;                // Set true if display colors are inverted
        cfg.rgb_order = false;            // Set true if red and blue color order is swapped
        cfg.dlen_16bit = false;           // Set true if data length is sent in 16-bit units
        cfg.bus_shared = true;            // Set true if SPI bus is shared with SD card

        _panel_instance.config(cfg);      // Apply panel configuration
      }

      {
        auto cfg = _touch_instance.config(); // Get touch controller configuration
        cfg.x_min = 0;                     // Minimum X coordinate
        cfg.x_max = 319;                   // Maximum X coordinate
        cfg.y_min = 0;                     // Minimum Y coordinate
        cfg.y_max = 479;                   // Maximum Y coordinate
        cfg.pin_int = 47;                  // Touch interrupt pin number
        cfg.bus_shared = false;            // Touch controller does not share bus
        cfg.offset_rotation = 0;           // Touch rotation offset

        // I2C connection configuration
        cfg.i2c_port = I2C_NUM_0;          // I2C port number
        cfg.pin_sda = GPIO_NUM_15;         // I2C SDA pin number
        cfg.pin_scl = GPIO_NUM_16;         // I2C SCL pin number
        cfg.pin_rst = 48;                  // Touch controller reset pin
        cfg.freq = 400000;                // I2C clock frequency (400kHz)
        cfg.i2c_addr = 0x14;               // I2C address (0x5D or 0x14)
        _touch_instance.config(cfg);       // Apply touch configuration
        _panel_instance.setTouch(&_touch_instance); // Attach touch controller to panel
      }

      setPanel(&_panel_instance);          // Set the panel instance for LGFX device
    }
};
