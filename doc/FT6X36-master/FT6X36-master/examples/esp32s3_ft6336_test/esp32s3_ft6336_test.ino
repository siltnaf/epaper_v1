#include <Arduino.h>
#include <Wire.h>
#include <FT6X36.h>

// ESP32-S3 <-> FT6336 wiring:
//   GPIO12 -> SCL
//   GPIO13 -> SDA
//   GPIO36 -> INT
//   3V3    -> VCC
//   GND    -> GND
constexpr int TOUCH_SCL_PIN = 12;
constexpr int TOUCH_SDA_PIN = 13;
constexpr int TOUCH_INT_PIN = 36;
constexpr uint32_t TOUCH_I2C_FREQUENCY = 400000;

FT6X36 touchPanel(&Wire, TOUCH_INT_PIN);

const char *eventName(TEvent event)
{
  switch (event)
  {
  case TEvent::TouchStart:
    return "TouchStart";
  case TEvent::TouchMove:
    return "TouchMove";
  case TEvent::TouchEnd:
    return "TouchEnd";
  case TEvent::Tap:
    return "Tap";
  case TEvent::DragStart:
    return "DragStart";
  case TEvent::DragMove:
    return "DragMove";
  case TEvent::DragEnd:
    return "DragEnd";
  default:
    return "None";
  }
}

bool readRegister(uint8_t reg, uint8_t &value)
{
  Wire.beginTransmission(FT6X36_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
  {
    return false;
  }

  if (Wire.requestFrom((uint8_t)FT6X36_ADDR, (uint8_t)1) != 1)
  {
    return false;
  }

  value = Wire.read();
  return true;
}

bool detectController()
{
  Wire.beginTransmission(FT6X36_ADDR);
  if (Wire.endTransmission() != 0)
  {
    Serial.println("ERROR: No I2C response at address 0x38.");
    Serial.println("Check 3.3 V power, GND, SDA GPIO13, and SCL GPIO12.");
    return false;
  }

  uint8_t chipId = 0;
  uint8_t vendorId = 0;
  uint8_t firmwareVersion = 0;

  if (!readRegister(FT6X36_REG_CHIPID, chipId) ||
      !readRegister(FT6X36_REG_PANEL_ID, vendorId) ||
      !readRegister(FT6X36_REG_FIRMWARE_VERSION, firmwareVersion))
  {
    Serial.println("ERROR: Controller answered, but its ID registers could not be read.");
    return false;
  }

  Serial.printf("Controller found at 0x%02X\n", FT6X36_ADDR);
  Serial.printf("Chip ID: 0x%02X (FT6336 expected: 0x%02X)\n", chipId, FT6336_CHIPID);
  Serial.printf("Vendor/panel ID: 0x%02X (expected: 0x%02X)\n", vendorId, FT6X36_VENDID);
  Serial.printf("Firmware version: 0x%02X\n", firmwareVersion);

  if (chipId != FT6336_CHIPID)
  {
    Serial.println("WARNING: The detected chip ID is not the expected FT6336 ID.");
  }

  return true;
}

void onTouch(TPoint point, TEvent event)
{
  Serial.print("X: ");
  Serial.print(point.x);
  Serial.print(", Y: ");
  Serial.print(point.y);
  Serial.print(", Event: ");
  Serial.print(eventName(event));
  Serial.print(", INT: ");
  Serial.println(digitalRead(TOUCH_INT_PIN));
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("ESP32-S3 FT6336 touch-panel test");
  Serial.printf("SDA=GPIO%d, SCL=GPIO%d, INT=GPIO%d\n",
                TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_INT_PIN);

  Wire.begin(TOUCH_SDA_PIN, TOUCH_SCL_PIN, TOUCH_I2C_FREQUENCY);
  Wire.setTimeOut(50);

  if (!detectController())
  {
    Serial.println("Touch test stopped.");
    while (true)
    {
      delay(1000);
    }
  }

  touchPanel.registerTouchHandler(onTouch);
  if (!touchPanel.begin())
  {
    Serial.println("ERROR: FT6X36 library initialization failed.");
    Serial.println("The chip ID or vendor/panel ID is not supported by this library.");
    while (true)
    {
      delay(1000);
    }
  }

  Serial.println("FT6336 initialized. Touch the panel to print X/Y coordinates.");
}

void loop()
{
  // The library's GPIO36 interrupt queues touch data for processing here.
  touchPanel.loop();
  delay(1);
}