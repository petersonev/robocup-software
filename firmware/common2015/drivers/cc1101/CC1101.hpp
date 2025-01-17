#pragma once

#include "mbed.h"
#include "CommLink.hpp"
#include "rtp.hpp"
#include "CC1101-Defines.hpp"

#define CCXXX1_DEBUG_MODE 0

/**
 * CC1101 Sub-1GHz Transceiver class
 */
class CC1101 : public CommLink {
public:
    /// Create a CC1101 hardware communication object
    CC1101(){};

    /** Create a CC1101 hardware communication object
     * @param mosi SPI data in pin
     * @param miso SPI data out pin
     * @param sck SPI clock pin
     * @param cs Chip select pin
     * @param int_pin Interrupt pin
     */
    CC1101(PinName mosi, PinName miso, PinName sck, PinName cs,
           PinName int_pin = NC);

    // These must have implementations in the CC1101 class since it is a derived
    // class of the base class CommLink
    /// Perform a soft reset for the transceiver
    void reset();

    int32_t selfTest();

    bool isConnected() const;

    /// Set the channel number
    void channel(uint16_t);

    /// Set an address for the transceiver used for packet filtering
    void address(uint8_t);

    /// Get the address of the transceiver
    uint8_t address();

    /// Get the transceiver's operating datarate
    uint16_t datarate();

    // The NOP command used to get the CC1101's status byte
    uint8_t status();
    uint8_t status(uint8_t);
    uint8_t lqi();
    uint8_t version();
    int16_t rssi();

    int32_t powerUp();

protected:
    // These must have implementations in the CC1101 class since it is a derived
    // class of the base class CommLink
    virtual int32_t sendData(uint8_t*, uint8_t);
    virtual int32_t getData(uint8_t*, uint8_t*);

    // Reading/Writing registers
    void write_reg(uint8_t, uint8_t);
    void write_reg(uint8_t, uint8_t*, uint8_t);
    uint8_t read_reg(uint8_t);
    void read_reg(uint8_t, uint8_t*, uint8_t);

    /// Send a command strobe to the CC1101 (1 byte SPI transfer)
    uint8_t strobe(uint8_t);

    // Send the TX or RX command strobe for placing the CC1101 in the respective
    // state
    void tx_mode();
    void rx_mode();
    void idle();

    // Send the command strobe to flush the TX or RX buffers on the CC1101
    void flush_tx();
    void flush_rx();

    void freq(uint32_t);
    void datarate(uint32_t);  // set data rate
    void put_rf_settings();
    void init();

private:
    void calibrate();
    void rssi(uint8_t);
    uint8_t mode();

    void assign_modem_params();
    void assign_packet_params();
    void interface_freq(uint32_t);
    void assign_channel_spacing(uint32_t);
    void set_rf_settings();
    void set_init_vars();
    void power_on_reset();

    rf_settings_t rfSettings;
    radio_state_t _mode;
    pck_ctrl_t _pck_control;
    modem_t _modem;

    uint16_t _channel;
    uint16_t _datarate;
    uint8_t _address;
    uint8_t _lqi;
    uint16_t _rssi;
    uint8_t _chip_version;
    uint32_t _base_freq;
};
