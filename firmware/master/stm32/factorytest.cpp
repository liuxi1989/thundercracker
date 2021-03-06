/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker firmware
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "factorytest.h"
#include "usart.h"
#include "board.h"
#include "macros.h"
#include "sysinfo.h"

#include "radio.h"
#include "nrf24l01.h"
#include "flash_device.h"
#include "usb/usbdevice.h"
#include "usbprotocol.h"
#include "volume.h"
#include "homebutton.h"
#include "powermanager.h"
#include "batterylevel.h"
#include "audiomixer.h"
#include "svmmemory.h"
#include "bootloader.h"
#include "cube.h"
#include "tasks.h"
#include "radioaddrfactory.h"
#include "nrf24l01.h"
#include "nrf8001/nrf8001.h"
#include "realtimeclock.h"

extern unsigned     __data_start;

FactoryTest::UartCommand FactoryTest::uartCommand;

FactoryTest::BtleTest FactoryTest::btleTest;

uint16_t FactoryTest::rfSuccessCount;
volatile uint16_t FactoryTest::rfTransmissionsRemaining;
RadioAddress FactoryTest::rfTestAddr;
uint8_t FactoryTest::rfTestAddrPrimaryChannel;
uint8_t FactoryTest::rfTestCubeVersion;

FactoryTest::TestHandler const FactoryTest::handlers[] = {
    nrfCommsHandler,            // 0
    flashCommsHandler,          // 1
    flashReadWriteHandler,      // 2
    ledHandler,                 // 3
    uniqueIdHandler,            // 4
    volumeCalibrationHandler,   // 5
    batteryCalibrationHandler,  // 6
    homeButtonHandler,          // 7
    shutdownHandler,            // 8
    audioTestHandler,           // 9
    bootloadRequestHandler,     // 10
    rfPacketTestHandler,        // 11
    rebootRequestHandler,       // 12
    getFirmwareVersion,         // 13
    rtcTestHandler,             // 14
    bleCommsHandler,            // 15
};

void FactoryTest::init()
{
}

/*
 * A new byte has arrived over the UART.
 * We're assembling a test command that looks like [len, opcode, params...]
 * If this byte overruns our buffer, reset our count of bytes received.
 * Otherwise, dispatch to the appropriate handler when a complete message
 * has been received.
 */
void FactoryTest::onUartIsr()
{
    uint8_t rxbyte;
    uint16_t status = Usart::Dbg.isr(rxbyte);

    if (status & Usart::STATUS_RXED) {

        uartCommand.append(rxbyte);
        if (uartCommand.complete())
            Tasks::trigger(Tasks::FactoryTest);
    }
}


void FactoryTest::task()
{
    /*
     * As the UART ISR runs at a reasonably high priority to avoid
     * getting overrun, we offload the handler execution to a task.
     */

    if (!uartCommand.complete())
        return;

    uint8_t opcode = uartCommand.opcode();
    if (opcode < arraysize(handlers)) {
        TestHandler handler = handlers[opcode];
        handler(uartCommand.len - 1, &uartCommand.buf[1]);
    }

    uartCommand.len = 0;
}

/*
 * Dispatch test commands via USB.
 * UsbMessages have a headers of UsbProtocol::HEADER_LEN bytes,
 * followed by a byte of test command, followed by payload data.
 */
void FactoryTest::usbHandler(const USBProtocolMsg &m)
{
    uint8_t cmd = m.payload[0];
    if (cmd < arraysize(handlers)) {
        TestHandler handler = handlers[cmd];
        // arg[0] is always the 'command type' byte
        handler(m.payloadLen(), m.payload);
    }
}

/*
 * Produce a packet of RF test data.
 *
 * Send any length packet made up of only "0x11" bytes. This does nothing more
 * than cycle the decompressor's write pointer around in a circle.
 * It's a two-nybble code for "seek forwards by 2 words".
 */
void FactoryTest::produce(PacketTransmission &tx)
{
    /*
     * The cube is listening on one of 2 addresses.
     * Since we don't have a good way of determining which one it's
     * listening on, send to both and treat a timeout on both as
     * a failure.
     */

    if (rfTestAddr.channel == rfTestAddrPrimaryChannel)
        RadioAddrFactory::convertPrimaryToAlternateChannel(rfTestAddr, rfTestCubeVersion);
    else
        rfTestAddr.channel = rfTestAddrPrimaryChannel;

    tx.dest = &rfTestAddr;
    tx.packet.len = 0;
    tx.numSoftwareRetries = 0;
    tx.numHardwareRetries = 0;

    uint8_t packetLen = MAX(1, rfTransmissionsRemaining % PacketBuffer::MAX_LEN);
    for (unsigned i = 0; i < packetLen; ++i)
        tx.packet.append(RF_TEST_BYTE);
}

/**************************************************************************
 * T E S T  H A N D L E R S
 *
 * Each handler is passed a list of arguments, the first of which is always
 * the command opcode itself.
 *
 **************************************************************************/

/*
 * len: 3
 * args[1] - radio tx power
 */
void FactoryTest::nrfCommsHandler(uint8_t argc, const uint8_t *args)
{
#ifdef USE_NRF24L01

    RadioManager::disableRadio();

    while (NRF24L01::instance.state() != NRF24L01::Idle) {
        Tasks::resetWatchdog();
    }

    uint8_t chan = args[1];
    NRF24L01::instance.setChannel(chan);
    const uint8_t response[] = { args[0], NRF24L01::instance.channel() };

    RadioManager::enableRadio();

    UsbDevice::write(response, sizeof response);
#endif
}

/*
 * len: 2
 * no args
 */
void FactoryTest::flashCommsHandler(uint8_t argc, const uint8_t *args)
{
    FlashDevice::JedecID id;
    FlashDevice::readId(&id);

    uint8_t result = 0;

#ifdef USE_W25Q256
    result = (id.manufacturerID == FlashDevice::WINBOND_MFGR_ID) ? 1 : 0;
#elif defined(USE_MX25L128)
    result = (id.manufacturerID == FlashDevice::MACRONIX_MFGR_ID) ? 1 : 0;
#else
#error "flash device part not specified"
#endif

    const uint8_t response[] = { args[0], result };
    UsbDevice::write(response, sizeof response);
}

/*
 * len: 4
 * args[1] - block number
 * args[2] - offset into block
 */
void FactoryTest::flashReadWriteHandler(uint8_t argc, const uint8_t *args)
{
    uint32_t ebAddr = args[1] * FlashDevice::ERASE_BLOCK_SIZE;
    uint32_t addr = ebAddr + args[2];

    FlashDevice::eraseBlock(ebAddr);

    const uint8_t txbuf[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    FlashDevice::write(addr, txbuf, sizeof txbuf);

    uint8_t rxbuf[sizeof txbuf];
    FlashDevice::read(addr, rxbuf, sizeof rxbuf);

    uint8_t result = (memcmp(txbuf, rxbuf, sizeof txbuf) == 0) ? 1 : 0;

    const uint8_t response[] = { args[0], result };
    UsbDevice::write(response, sizeof response);
}

/*
 * args[1] - color, bit0 == green, bit1 == red
 */
void FactoryTest::ledHandler(uint8_t argc, const uint8_t *args)
{
    GPIOPin green = LED_GREEN_GPIO;
    green.setControl(GPIOPin::OUT_2MHZ);

    GPIOPin red = LED_RED_GPIO;
    red.setControl(GPIOPin::OUT_2MHZ);

    uint8_t status = args[1];

    if (status & 1)
        green.setLow();
    else
        green.setHigh();

    if (status & (1 << 1))
        red.setLow();
    else
        red.setHigh();

    // no result - just respond to indicate that we're done
    const uint8_t response[] = { args[0] };
    UsbDevice::write(response, sizeof response);
}

/*
 * No args - just return hw id.
 * Special case: pass an extra argument to specify that the response should
 *               be sent via USB.
 */
void FactoryTest::uniqueIdHandler(uint8_t argc, const uint8_t *args)
{
    uint8_t response[2 + SysInfo::UniqueIdNumBytes] = { sizeof response, args[0] };
    memcpy(response + 2, SysInfo::UniqueId, SysInfo::UniqueIdNumBytes);

    if (argc >= 2)
        UsbDevice::write(&response[1], sizeof response - 1);
    else
        Usart::Dbg.write(response, sizeof response);
}

/*
 * args[1]: position, 0 = low extreme, non-zero = high extreme
 *
 * response: test id, calibration state, 16-bit raw reading stored as calibration.
 */
void FactoryTest::volumeCalibrationHandler(uint8_t argc, const uint8_t *args)
{
    Volume::CalibrationState cs = args[1] ? Volume::CalibrationHigh : Volume::CalibrationLow;
    uint32_t rawValue = Volume::calibrate(cs);

    const uint8_t response[] = { args[0], args[1],
        uint8_t(rawValue), uint8_t(rawValue >> 8), uint8_t(rawValue >> 16), uint8_t(rawValue >> 24) };

    UsbDevice::write(response, sizeof response);
}

/*
 * Special case: pass an extra argument to specify that the response should
 *               be sent via USB.
 */
void FactoryTest::batteryCalibrationHandler(uint8_t argc, const uint8_t *args)
{
    uint32_t vsys = BatteryLevel::vsys();
    uint32_t vraw = BatteryLevel::raw();
    uint32_t vscl = BatteryLevel::scaled();

    const uint8_t response[14] = { sizeof response , args[0],
        uint8_t(vsys), uint8_t(vsys >> 8), uint8_t(vsys >> 16), uint8_t(vsys >> 24),
        uint8_t(vraw), uint8_t(vraw >> 8), uint8_t(vraw >> 16), uint8_t(vraw >> 24),
        uint8_t(vscl), uint8_t(vscl >> 8), uint8_t(vscl >> 16), uint8_t(vscl >> 24),
    };

    if (argc >= 2)
        UsbDevice::write(&response[1], sizeof response - 1);
    else
        Usart::Dbg.write(response, sizeof response);
}

/*
 * no args - we just report the state of the home button.
 */
void FactoryTest::homeButtonHandler(uint8_t argc, const uint8_t *args)
{
    const uint8_t buttonState = HomeButton::isPressed() ? 1 : 0;

    const uint8_t response[] = { args[0], buttonState };
    UsbDevice::write(response, sizeof response);
}

void FactoryTest::shutdownHandler(uint8_t argc, const uint8_t *args)
{
    const uint8_t response[] = { args[0] };
    UsbDevice::write(response, sizeof response);
    // write packet again so we know the first one was transmitted
    UsbDevice::write(response, sizeof response);

    PowerManager::batteryPowerOff();
}



/*
 * args[1] - non-zero == start, zero == stop
 */

void FactoryTest::audioTestHandler(uint8_t argc, const uint8_t *args)
{
    AudioMixer::instance.stop(0); // make sure we're stopped in either case.
    if (args[1]) {

        /*
         * Audio data is expected to flow through the SVM virtual memory
         * system. We'll copy a small triangle wave's data to user RAM
         * (assuming nothing is running there)
         */

        const int16_t TriangleData[] = { 0x7FFF, int16_t(0x8000) };

        SvmMemory::VirtAddr triangleDataVA = SvmMemory::VIRTUAL_RAM_BASE;
        SvmMemory::PhysAddr triangleDataPA;
        SvmMemory::mapRAM(triangleDataVA, sizeof TriangleData, triangleDataPA);
        memcpy(triangleDataPA, TriangleData, sizeof TriangleData);

        const _SYSAudioModule Triangle = {
            /* sampleRate */ 262,   // near enough to C-4 (261.626Hz)
            /* loopStart  */ 0,
            /* loopEnd    */ arraysize(TriangleData),
            /* loopType   */ _SYS_LOOP_REPEAT,
            /* type       */ _SYS_PCM,
            /* volume     */ _SYS_AUDIO_MAX_VOLUME,
            /* dataSize   */ sizeof TriangleData,
            /* pData      */ triangleDataVA,
        };

        AudioMixer::instance.play(&Triangle, 0, _SYS_LOOP_REPEAT);
    }

    // no real response - just indicate that we've taken the requested action
    const uint8_t response[] = { args[0], args[1] };
    UsbDevice::write(response, sizeof response);
}

/*
 * XXX: find a better place for this handler? This has nothing to do with FactoryTest
 *      but none of the other USB subsystems seem like much better options.
 */
void FactoryTest::bootloadRequestHandler(uint8_t argc, const uint8_t *args)
{
#ifdef BOOTLOADABLE
    __data_start = Bootloader::UPDATE_REQUEST_KEY;
    NVIC.deinit();
    NVIC.systemReset();
#endif
}

/*
 * XXX: This one could use a better home, for the same reason.
 */
void FactoryTest::rebootRequestHandler(uint8_t argc, const uint8_t *args)
{
    NVIC.deinit();
    NVIC.systemReset();
}

/*
 * args[1:2] -- uint16 transmission count
 * args[3:10] -- uint64_t hwid of cube to test
 */
void FactoryTest::rfPacketTestHandler(uint8_t argc, const uint8_t *args)
{
    uint64_t hwid;
    memcpy(&hwid, &args[3], sizeof hwid);
    RadioAddrFactory::fromHardwareID(rfTestAddr, hwid);
    rfTestAddrPrimaryChannel = rfTestAddr.channel;
    rfTestCubeVersion = hwid & 0xff;

#ifdef USE_NRF24L01
    NRF24L01::setRfTestEnabled(true);

    // multiply transmission count by 2 since we're sending
    // each attempt to both channels a cube might be listening on
    rfTransmissionsRemaining = *reinterpret_cast<const uint16_t*>(&args[1]) * 2;

    while (rfTransmissionsRemaining)
        Tasks::waitForInterrupt();

    NRF24L01::setRfTestEnabled(false);
#endif

    /*
     * Respond with the number of packets sent, and the number of successful transmissions
     */
    const uint8_t report[] = { args[0], args[1], args[2],
                               uint8_t(rfSuccessCount), uint8_t(rfSuccessCount >> 8) };
    UART_HEX(rfSuccessCount);
    UART("\r\n");
    UsbDevice::write(report, sizeof report);
    rfSuccessCount = 0;
}

/*
 *
 */
void FactoryTest::rtcTestHandler(uint8_t argc, const uint8_t *args)
{
    uint8_t start_count = RealTimeClock::count();

    SysTime::Ticks deadline = SysTime::ticks() + SysTime::sTicks(args[1]);
    while(SysTime::ticks() < deadline);

    uint8_t count_diff = RealTimeClock::count() -  start_count;

    const uint8_t report[] = {args[0], count_diff};

    UsbDevice::write(report, sizeof report);
}

/*
 *  no args
 */
void FactoryTest::getFirmwareVersion(uint8_t argc, const uint8_t *args)
{
    const uint8_t MAX_SIZE = 32;
    const uint8_t sz = MIN( MAX_SIZE, strlen(TOSTRING(SDK_VERSION)));
    uint8_t response[MAX_SIZE] = { args[0] };
    memcpy(&response[1], TOSTRING(SDK_VERSION), sz);

    UsbDevice::write(response, sz+1);
}

/*
 *  args[1] -- NRF8001::TestPhase to enter
 *  args[2] -- DTM packet format
 *  args[3] -- DTM packet length
 *  args[4] -- DTM packet frequency
 */
void FactoryTest::bleCommsHandler(uint8_t argc, const uint8_t *args)
{
    uint8_t phase = args[1];
    uint8_t pkt, len, freq;
    if (argc >= 5) {
        pkt = args[2];
        len = args[3];
        freq = args[4];
    } else {
        pkt = len = freq = 0;
    }

#ifdef HAVE_NRF8001
    btleTest.inProgress = 1;
    NRF8001::instance.test(phase, pkt, len, freq);
    switch (phase) {
        case NRF8001::EnterTestMode:
        case NRF8001::ExitTestMode:
            while (btleTest.inProgress) {
                Tasks::waitForInterrupt();
            }
            break;

        default:
            break;
    }
#endif

    const uint8_t report[] = { args[0], phase,
                               btleTest.status,
                               uint8_t(btleTest.result),
                               uint8_t(btleTest.result >> 8) };
    UsbDevice::write(report, sizeof report);
}

IRQ_HANDLER ISR_FN(UART_DBG)()
{
    FactoryTest::onUartIsr();
}
