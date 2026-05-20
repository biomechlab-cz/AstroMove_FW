//////////////////////////////////////////////////////////////////////////////////////////
//
//   Arduino Library for ADS1292R Shield/Breakout
//
//   Copyright (c) 2017 ProtoCentral
//
//   This software is licensed under the MIT License(http://opensource.org/licenses/MIT).
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
//   NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
//   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//   Requires g4p_control graphing library for processing.  Built on V4.1
//   Downloaded from Processing IDE Sketch->Import Library->Add Library->G4P Install
//
/////////////////////////////////////////////////////////////////////////////////////////
#include "Arduino.h"
#include "protocentralAds1292r.h"
#include <SPI.h>

int j, i;

volatile byte SPI_RX_Buff[15];
volatile static int SPI_RX_Buff_Count = 0;
volatile char *SPI_RX_Buff_Ptr;

unsigned long resultTemp = 0;

long statusByte = 0;

uint8_t LeadStatus = 0;

int32_t ads1292r::interpret24bitAsInt32(uint8_t byte24_16, uint8_t byte16_8, uint8_t byte8_0)
{
  int32_t newInt = (((0xFF & byte24_16) << 16) | ((0xFF & byte16_8) << 8) | (0xFF & byte8_0));
  if ((newInt & 0x00800000) > 0)
  {
    newInt |= 0xFF000000;
  }
  else
  {
    newInt &= 0x00FFFFFF;
  }
  return newInt;
}

boolean ads1292r::getAds1292EcgAndRespirationSamples(const int dataReady, const int chipSelect, ads1292OutputValues *ecgRespirationValues)
{
  SPI_RX_Buff_Ptr = ads1292ReadData(chipSelect); // Read the data,point the data to a pointer

  for (int i = 0; i < 9; i++)
  {
    SPI_RX_Buff[SPI_RX_Buff_Count++] = *(SPI_RX_Buff_Ptr + i); // store the result data in array
  }

  (ecgRespirationValues->sDaqVals)[1] = interpret24bitAsInt32(SPI_RX_Buff[6], SPI_RX_Buff[7], SPI_RX_Buff[8]);

  statusByte = (long)((long)SPI_RX_Buff[2] | ((long)SPI_RX_Buff[1]) << 8 | ((long)SPI_RX_Buff[0]) << 16); // First 3 bytes represents the status
  statusByte = (statusByte & 0x0f8000) >> 15;                                                             // bit15 gives the lead status
  LeadStatus = (unsigned char)statusByte;
  resultTemp = (uint32_t)((0 << 24) | (SPI_RX_Buff[3] << 16) | SPI_RX_Buff[4] << 8 | SPI_RX_Buff[5]); // 6,7,8
  resultTemp = (uint32_t)(resultTemp << 8);
  ecgRespirationValues->sresultTempResp = (long)(resultTemp);

  if (!((LeadStatus & 0x1f) == 0))
  {
    ecgRespirationValues->leadoffDetected = true;
  }

  else
  {
    ecgRespirationValues->leadoffDetected = false;
  }

  SPI_RX_Buff_Count = 0;
  return true;
}

char *ads1292r::ads1292ReadData(const int chipSelect)
{
  static char SPI_Dummy_Buff[10];
  digitalWrite(chipSelect, LOW);

  for (int i = 0; i < 9; ++i)
  {
    SPI_Dummy_Buff[i] = SPI.transfer(CONFIG_SPI_MASTER_DUMMY);
  }

  digitalWrite(chipSelect, HIGH);
  return SPI_Dummy_Buff;
}

void ads1292r::ads1292Init(const int chipSelect, const int pwdnPin, const int startPin, const int dataReadyPin)
{

  ads1292Reset(pwdnPin);
  delay(100);
  ads1292DisableStart(startPin);
  ads1292EnableStart(startPin);
  ads1292HardStop(startPin);
  ads1292StartDataConvCommand(chipSelect);
  ads1292SoftStop(chipSelect);
  delay(50);
  ads1292StopReadDataContinuous(chipSelect); // SDATAC command
  delay(300);
//   ads1292RegWrite(ADS1292_REG_CONFIG1, SPS_1000, chipSelect);
//   delay(10);
//   // bit7 must be 1, bit5 reference buffer
//   ads1292RegWrite(ADS1292_REG_CONFIG2,  0b10100000, chipSelect); // Lead-off comp off, test signal disabled
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFF,     0b00010000,       chipSelect); // LOFF OFF
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH1SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH1SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH2SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH2SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
  
//   ads1292RegWrite(ADS1292_REG_RLDSENS,  0x00,       chipSelect);
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFFSENS, 0x00,       chipSelect);
//   delay(10);
//   // ads1292RegWrite(ADS1292_REG_LOFFSTAT, 0x00,       chipSelect); // LOFF settings: all disabled
//   // delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP1,    0b00000010, chipSelect); // Respiration: MOD/DEMOD turned off
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP2,    0b00000001, chipSelect); // Respiration: Calib OFF, respiration freq defaults
//   delay(10);


//////////////////////////////////////////////////////////////////////////////////
////////////////////////// EEG - very strong candidate ///////////////////////////
//////////////////////////////////////////////////////////////////////////////////
//   ads1292RegWrite(ADS1292_REG_CONFIG1, SPS_1000, chipSelect);
//   delay(10);
//   // bit7 must be 1, bit5 reference buffer
//   ads1292RegWrite(ADS1292_REG_CONFIG2,  0b10100000, chipSelect); // Lead-off comp off, test signal disabled
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFF,     0b00010000,       chipSelect); // LOFF OFF
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH1SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH1SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH2SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH2SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
  
//   ads1292RegWrite(ADS1292_REG_RLDSENS,  0b00100011,       chipSelect);
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFFSENS, 0x0F,       chipSelect);
//   delay(10);
//   // ads1292RegWrite(ADS1292_REG_LOFFSTAT, 0x00,       chipSelect); // LOFF settings: all disabled
//   // delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP1,    0b00000010, chipSelect); // Respiration: MOD/DEMOD turned off
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP2,    0b00000001, chipSelect); // Respiration: Calib OFF, respiration freq defaults
//   delay(10);

//////////////////////////////////////////////////////////////////////////////////
////////////////////////// EEG - very strong candidate ///////////////////////////
//////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////
/////////////////////// V/Ohm - very strong candidate 2 //////////////////////////
//////////////////////////////////////////////////////////////////////////////////
  ads1292RegWrite(ADS1292_REG_CONFIG1, SPS_1000, chipSelect);
  delay(10);
  // bit7 must be 1, bit5 reference buffer
  ads1292RegWrite(ADS1292_REG_CONFIG2,  0b10100000, chipSelect); // Lead-off comp off, test signal disabled
  delay(10);
  ads1292RegWrite(ADS1292_REG_LOFF,     0b00010000,       chipSelect); // LOFF OFF
  delay(10);
//  ads1292RegWrite(ADS1292_REG_CH1SET,   0b00000000, chipSelect); // PGA 6x
  ads1292RegWrite(ADS1292_REG_CH1SET,   0b01000000, chipSelect); // PGA 8x
  delay(10);
//  ads1292RegWrite(ADS1292_REG_CH2SET,   0b00000000, chipSelect); // PGA 6x
  ads1292RegWrite(ADS1292_REG_CH2SET,   0b01100000, chipSelect); // PGA 8x
  delay(10);
  
  ads1292RegWrite(ADS1292_REG_RLDSENS,  0x00,       chipSelect);
  delay(10);
  ads1292RegWrite(ADS1292_REG_LOFFSENS, 0x0F,       chipSelect);
  delay(10);
  // ads1292RegWrite(ADS1292_REG_LOFFSTAT, 0x00,       chipSelect); // LOFF settings: all disabled
  // delay(10);
  ads1292RegWrite(ADS1292_REG_RESP1,    0b11110010, chipSelect); // Respiration: MOD/DEMOD turned off
  delay(10);
  ads1292RegWrite(ADS1292_REG_RESP2,    0b00000011, chipSelect); // Respiration: Calib OFF, respiration freq defaults
  delay(10);

//////////////////////////////////////////////////////////////////////////////////
/////////////////////// V/Ohm - very strong candidate 2 //////////////////////////
//////////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// ECG - artefacts /{{{{{{{/////////////////////////
//////////////////////////////////////////////////////////////////////////////////
  ads1292RegWrite(ADS1292_REG_CONFIG1, SPS_1000, chipSelect);
//   delay(10);
//   // bit7 must be 1, bit5 reference buffer
//   ads1292RegWrite(ADS1292_REG_CONFIG2,  0b10100000, chipSelect); // Lead-off comp off, test signal disabled
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFF,     0b00010000,       chipSelect); // LOFF OFF
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH1SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH1SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
// //  ads1292RegWrite(ADS1292_REG_CH2SET,   0b00000000, chipSelect); // PGA 6x
//   ads1292RegWrite(ADS1292_REG_CH2SET,   0b01100000, chipSelect); // PGA 8x
//   delay(10);
  
//   ads1292RegWrite(ADS1292_REG_RLDSENS,  0b00001100,       chipSelect);
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_LOFFSENS, 0x00,       chipSelect);
//   delay(10);
//   // ads1292RegWrite(ADS1292_REG_LOFFSTAT, 0x00,       chipSelect); // LOFF settings: all disabled
//   // delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP1,    0b00000010, chipSelect); // Respiration: MOD/DEMOD turned off
//   delay(10);
//   ads1292RegWrite(ADS1292_REG_RESP2,    0b00000001, chipSelect); // Respiration: Calib OFF, respiration freq defaults
//   delay(10);

//////////////////////////////////////////////////////////////////////////////////
//////////////////////////////// ECG - artefacts /{{{{{{{/////////////////////////
//////////////////////////////////////////////////////////////////////////////////


  ads1292StartReadDataContinuous(chipSelect);
  delay(10);
  ads1292EnableStart(startPin);
  // TODO attach Interrupt here
}

// NOTUSED
void ads1292r::interruptDataReady()
{
  lastTime = micros();
  // TODO switch this for semafor
  dataReadyFlag = true;
}

// NOTUSED
void ads1292r::addSem(SemaphoreHandle_t *sem)
{
  adcSemaphore = sem;
}

void ads1292r::ads1292Reset(const int pwdnPin)
{
  digitalWrite(pwdnPin, HIGH);
  delay(100); // Wait 100 mSec
  digitalWrite(pwdnPin, LOW);
  delay(100);
  digitalWrite(pwdnPin, HIGH);
  delay(100);
}

void ads1292r::ads1292DisableStart(const int startPin)
{
  digitalWrite(startPin, LOW);
  delay(20);
}

void ads1292r::ads1292EnableStart(const int startPin)
{
  digitalWrite(startPin, HIGH);
  delay(20);
}

void ads1292r::ads1292HardStop(const int startPin)
{
  digitalWrite(startPin, LOW);
  delay(100);
}

void ads1292r::ads1292StartDataConvCommand(const int chipSelect)
{
  ads1292SPICommandData(START, chipSelect); // Send 0x08 to the ADS1x9x
}

void ads1292r::ads1292SoftStop(const int chipSelect)
{
  ads1292SPICommandData(STOP, chipSelect); // Send 0x0A to the ADS1x9x
}

void ads1292r::ads1292StartReadDataContinuous(const int chipSelect)
{
  ads1292SPICommandData(RDATAC, chipSelect); // Send 0x10 to the ADS1x9x
}

void ads1292r::ads1292StopReadDataContinuous(const int chipSelect)
{
  ads1292SPICommandData(SDATAC, chipSelect); // Send 0x11 to the ADS1x9x
}

void ads1292r::ads1292SPICommandData(unsigned char dataIn, const int chipSelect)
{
  byte data[1];
  // data[0] = dataIn;
  digitalWrite(chipSelect, LOW);
  delay(2);
  digitalWrite(chipSelect, HIGH);
  delay(2);
  digitalWrite(chipSelect, LOW);
  delay(2);
  SPI.transfer(dataIn);
  delay(2);
  digitalWrite(chipSelect, HIGH);
}

// Sends a write command to SCP1000
void ads1292r::ads1292RegWrite(unsigned char READ_WRITE_ADDRESS, unsigned char DATA, const int chipSelect)
{
  // this is checking for bits that have to be set according to the datasheet
  switch (READ_WRITE_ADDRESS)
  {
  case 1:
    DATA = DATA & 0x87;
    break;
  case 2:
    DATA = DATA & 0xFB;
    DATA |= 0x80;
    break;
  case 3:
    DATA = DATA & 0xFD;
    DATA |= 0x10;
    break;
  case 7:
    DATA = DATA & 0x3F;
    break;
  case 8:
    DATA = DATA & 0x5F;
    break;
  case 9:
    DATA |= 0x02;
    break;
  case 10:
    DATA = DATA & 0x87;
    DATA |= 0x01;
    break;
  case 11:
    DATA = DATA & 0x0F;
    break;
  default:
    break;
  }
  // now combine the register address and the command into one byte:
  byte dataToSend = READ_WRITE_ADDRESS | WREG;
  digitalWrite(chipSelect, LOW);
  delay(2);
  digitalWrite(chipSelect, HIGH);
  delay(2);
  // take the chip select low to select the device:
  digitalWrite(chipSelect, LOW);
  delay(2);
  SPI.transfer(dataToSend); // Send register location
  SPI.transfer(0x00);       // number of register to wr
  SPI.transfer(DATA);       // Send value to record into register
  delay(2);
  // take the chip select high to de-select:
  digitalWrite(chipSelect, HIGH);
}
