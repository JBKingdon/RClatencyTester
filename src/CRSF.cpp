#include <Arduino.h>
#include "CRSF.h"
#include "HardwareSerial.h"

#include "SoftwareSerial.h"
extern SoftwareSerial usbSerial;

volatile bool CRSF::ignoreSerialData = false;
volatile bool CRSF::CRSFframeActive = false; //since we get a copy of the serial data use this flag to know when to ignore it

void inline CRSF::nullCallback(void){};

void (*CRSF::RCdataCallback)() = &nullCallback; // null placeholder callback

void (*CRSF::disconnected)() = &nullCallback; // called when CRSF stream is lost
void (*CRSF::connected)() = &nullCallback;    // called when CRSF stream is regained

/// UART Handling ///
bool CRSF::CRSFstate = false;
uint32_t CRSF::UARTcurrentBaud = CRSF_OPENTX_FAST_BAUDRATE;
uint32_t CRSF::UARTrequestedBaud = CRSF_OPENTX_FAST_BAUDRATE;
uint32_t CRSF::GoodPktsCount = 0;
uint32_t CRSF::BadPktsCount = 0;

volatile uint8_t CRSF::SerialInPacketLen = 0; // length of the CRSF packet as measured
volatile uint8_t CRSF::SerialInPacketPtr = 0; // index where we are reading/writing

volatile uint16_t CRSF::ChannelDataIn[16] = {0};

volatile inBuffer_U CRSF::inBuffer;

volatile uint32_t CRSF::RCdataLastRecv = 0;

void CRSF::Begin()
{
    usbSerial.println("About to start CRSF task...");
}

void ICACHE_RAM_ATTR CRSF::handleUARTin() //RTOS task to read and write CRSF packets to the serial port
{
    volatile uint8_t *SerialInBuffer = CRSF::inBuffer.asUint8_t;

    if (this->_dev->available())
    {
        char inChar = this->_dev->read();

        if (CRSFframeActive == false)
        {
            // stage 1 wait for sync byte //
            if (inChar == CRSF_ADDRESS_CRSF_TRANSMITTER || inChar == CRSF_SYNC_BYTE) // we got sync, reset write pointer
            {
                SerialInPacketPtr = 0;
                SerialInPacketLen = 0;
                CRSFframeActive = true;
                SerialInBuffer[SerialInPacketPtr] = inChar;
                SerialInPacketPtr++;
            }
        }
        else // frame is active so we do the processing
        {
            // first if things have gone wrong //
            if (SerialInPacketPtr > CRSF_MAX_PACKET_LEN - 1) // we reached the maximum allowable packet length, so start again because shit fucked up hey.
            {
                SerialInPacketPtr = 0;
                SerialInPacketLen = 0;
                CRSFframeActive = false;
                return;
            }

            // special case where we save the expected pkt len to buffer //
            if (SerialInPacketPtr == 1)
            {
                if (inChar <= CRSF_MAX_PACKET_LEN)
                {
                    SerialInPacketLen = inChar;
                }
                else
                {
                    SerialInPacketPtr = 0;
                    SerialInPacketLen = 0;
                    CRSFframeActive = false;
                    return;
                }
            }

            SerialInBuffer[SerialInPacketPtr] = inChar;
            SerialInPacketPtr++;

            if (SerialInPacketPtr == SerialInPacketLen + 2) // plus 2 because the packlen is referenced from the start of the 'type' flag, IE there are an extra 2 bytes.
            {
                char CalculatedCRC = CalcCRC((uint8_t *)SerialInBuffer + 2, SerialInPacketPtr - 3);

                if (CalculatedCRC == inChar)
                {
                    ProcessPacket();
                    GoodPktsCount++;
                }
                else
                {
                    CRSFframeActive = false;
                    SerialInPacketPtr = 0;
                    SerialInPacketLen = 0;
                    while (this->_dev->available())
                    {
                        this->_dev->read(); // empty any remaining garbled data
                    }
                    BadPktsCount++;
                }
                SerialInPacketPtr = 0;
                SerialInPacketLen = 0;
                CRSFframeActive = false;
            }
        }
    }
}

bool ICACHE_RAM_ATTR CRSF::ProcessPacket()
{
    if (CRSFstate == false)
    {
        CRSFstate = true;
        usbSerial.println("CRSF UART Connected");
        connected();
    }

    const uint8_t packetType = CRSF::inBuffer.asRCPacket_t.header.type;

    // usbSerial.print("I");

    if (packetType == CRSF_FRAMETYPE_ELRS_RC_DB)
    {
        // usbSerial.print("D");
        CRSF::RCdataLastRecv = micros();
        GetChannelDataInDB();
        (RCdataCallback)(); // run new RC data callback
        return true;

    } else if (packetType == CRSF_FRAMETYPE_RC_CHANNELS_PACKED) {

        // usbSerial.print("P");

        CRSF::RCdataLastRecv = micros();
        GetChannelDataIn();
        (RCdataCallback)(); // run new RC data callback
        return true;
    } else {
        // usbSerial.printf("%d ", packetType);
    }
    return false;
}

void ICACHE_RAM_ATTR CRSF::GetChannelDataIn() // data is packed as 11 bits per channel
{
    #define SERIAL_PACKET_OFFSET 3
    const volatile crsf_channels_t *rcChannels = &CRSF::inBuffer.asRCPacket_t.channels;
    ChannelDataIn[0] = (rcChannels->ch0);
    ChannelDataIn[1] = (rcChannels->ch1);
    ChannelDataIn[2] = (rcChannels->ch2);
    ChannelDataIn[3] = (rcChannels->ch3);
}

void ICACHE_RAM_ATTR CRSF::GetChannelDataInDB() 
{
    const volatile crsf_elrs_channels_DB_t *rcChannels = &CRSF::inBuffer.asRCPacket_t.channelsDB;
    ChannelDataIn[0] = (rcChannels->chan0) >> 1;
    ChannelDataIn[1] = (rcChannels->chan1) >> 1;
    ChannelDataIn[2] = (rcChannels->chan2) >> 1;
    ChannelDataIn[3] = (rcChannels->chan3) >> 1;

}

