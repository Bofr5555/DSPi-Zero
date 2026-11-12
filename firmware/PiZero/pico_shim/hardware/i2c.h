#pragma once

#include "hardware/address_mapped.h"

// RP2350 I2C API, for DSPi's I2C control interface and Control Surfaces display. Two RAM stand-ins
// that never move a byte: no data to read and no room to write, so DSPi's display pump sees a stall.
typedef struct {
    io_rw_32 con;
    io_rw_32 tar;
    io_rw_32 sar;
    uint32_t _pad0;
    io_rw_32 data_cmd;
    uint32_t _pad1[27];
    io_ro_32 tx_abrt_source;
    uint32_t _pad2[5];
    io_ro_32 clr_tx_abrt;
    io_rw_32 enable;
} i2c_hw_t;

typedef struct i2c_inst i2c_inst_t;
extern i2c_hw_t pico_shim_i2c_hw[2];
#define i2c0 ((i2c_inst_t *)&pico_shim_i2c_hw[0])
#define i2c1 ((i2c_inst_t *)&pico_shim_i2c_hw[1])

#define I2C0_IRQ 36u
#define I2C1_IRQ 37u

static inline i2c_hw_t *i2c_get_hw(i2c_inst_t *i2c) {
    return (i2c_hw_t *)i2c;
}
static inline uint i2c_get_index(i2c_inst_t *i2c) {
    return i2c == i2c1 ? 1u : 0u;
}

uint i2c_init(i2c_inst_t *i2c, uint baudrate);
void i2c_deinit(i2c_inst_t *i2c);
size_t i2c_get_write_available(i2c_inst_t *i2c);
size_t i2c_get_read_available(i2c_inst_t *i2c);
uint8_t i2c_read_byte_raw(i2c_inst_t *i2c);
void i2c_write_byte_raw(i2c_inst_t *i2c, uint8_t value);
