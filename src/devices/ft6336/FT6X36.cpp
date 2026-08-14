#include "devices/ft6336/FT6X36.h"

FT6X36 *FT6X36::_instance = nullptr;

FT6X36::FT6X36(TwoWire *wire, int8_t intPin)
{
	_instance = this;
	_wire = wire;
	_intPin = intPin;
}

FT6X36::~FT6X36()
{
	end();
	if (_instance == this)
		_instance = nullptr;
}

void ISR_ATTR FT6X36::isr()
{
	if (_instance)
		_instance->onInterrupt();
}

bool FT6X36::begin(uint8_t threshold)
{
	end();
	if (_wire == nullptr || _intPin < 0)
		return false;

	_chipId = readRegister8(FT6X36_REG_CHIPID);
	_vendorId = readRegister8(FT6X36_REG_PANEL_ID);
	_firmwareVersion = readRegister8(FT6X36_REG_FIRMWARE_VERSION);
	if (_chipId != FT6206_CHIPID && _chipId != FT6236_CHIPID && _chipId != FT6336_CHIPID)
		return false;

	if (!writeRegister8(FT6X36_REG_DEVICE_MODE, 0x00) ||
		!writeRegister8(FT6X36_REG_THRESHHOLD, threshold) ||
		!writeRegister8(FT6X36_REG_TOUCHRATE_ACTIVE, 0x0E) ||
		!writeRegister8(FT6X36_REG_INTERRUPT_MODE, 0x01))
		return false;

	_isrCounter = 0;
	_touchActive = false;
	_dragMode = false;
	_tapFiredOnPress = false;
	pinMode(_intPin, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(_intPin), FT6X36::isr, FALLING);
	_interruptAttached = true;
	_online = true;
	return true;
}

void FT6X36::end()
{
	if (_interruptAttached)
	{
		detachInterrupt(digitalPinToInterrupt(_intPin));
		_interruptAttached = false;
	}
	_online = false;
	_isrCounter = 0;
	_touchActive = false;
	_dragMode = false;
	_tapFiredOnPress = false;
}

bool FT6X36::isOnline() const
{
	return _online;
}

uint8_t FT6X36::chipId() const
{
	return _chipId;
}

uint8_t FT6X36::vendorId() const
{
	return _vendorId;
}

uint8_t FT6X36::firmwareVersion() const
{
	return _firmwareVersion;
}

void FT6X36::registerIsrHandler(void (*fn)())
{
	_isrHandler = fn;
}

void FT6X36::registerTouchHandler(void (*fn)(TPoint point, TEvent e))
{
	_touchHandler = fn;
}

uint8_t FT6X36::touched()
{
	uint8_t n = readRegister8(FT6X36_REG_NUM_TOUCHES);
	if (n > 2)
	{
		n = 0;
	}
	return n;
}

void FT6X36::loop()
{
	if (!_online)
		return;

	uint8_t pending = 0;
	noInterrupts();
	pending = _isrCounter;
	_isrCounter = 0;
	interrupts();

	while (pending-- > 0)
	{
		processTouch();
	}

	// Movement samples are not guaranteed to produce a falling edge on every
	// FT6336 panel. Once a touch starts, poll continuously until LiftUp so a
	// finger sweep has enough Contact positions for reliable direction/distance
	// classification. Also poll while INT is held low for edge-less revisions.
	if ((_touchActive || digitalRead(_intPin) == LOW) &&
		millis() - _lastPollTime >= 10)
	{
		_lastPollTime = millis();
		processTouch();
	}
}

void FT6X36::processTouch()
{
	if (!_online)
		return;
	if (!readData() || _touches > 2)
		return;
	uint8_t n = 0;
	TRawEvent event = (TRawEvent)_touchEvent[n];
	TPoint point{_touchX[n], _touchY[n]};
	if (_touches == 0 && event != TRawEvent::LiftUp)
	{
		if (!_touchActive)
			return;
		event = TRawEvent::LiftUp;
		point = _pointIdx > 0 ? _points[_pointIdx - 1] : _points[0];
	}

	if (event == TRawEvent::PressDown)
	{
		if (!_touchActive)
		{
			_points[0] = point;
			_pointIdx = 1;
			_dragMode = false;
			_touchActive = true;
			_touchStartTime = millis();
			fireEvent(point, TEvent::TouchStart);
			// Wait until LiftUp before classifying the interaction. Firing Tap here
			// opens list rows before a finger sweep can be recognized as a swipe.
			_tapFiredOnPress = false;
		}
		else
		{
			if (_pointIdx < 10)
				_points[_pointIdx++] = point;
			fireEvent(point, TEvent::TouchMove);
		}
	}
	else if (event == TRawEvent::Contact)
	{
		if (!_touchActive)
		{
			_points[0] = point;
			_pointIdx = 1;
			_dragMode = false;
			_touchActive = true;
			_touchStartTime = millis();
			fireEvent(point, TEvent::TouchStart);
			_tapFiredOnPress = false;
		}
		if (_pointIdx < 10)
		{
			_points[_pointIdx] = point;
			_pointIdx += 1;
		}
		if (!_dragMode && _points[0].aboutEqual(point) && millis() - _touchStartTime > 300)
		{
			_dragMode = true;
			fireEvent(point, TEvent::DragStart);
		}
		else if (_dragMode)
			fireEvent(point, TEvent::DragMove);

		fireEvent(point, TEvent::TouchMove);
	}
	else if (event == TRawEvent::LiftUp)
	{
		if (!_touchActive)
			return;
		_points[9] = point;
		_touchEndTime = millis();
		fireEvent(point, TEvent::TouchEnd);
		if (_dragMode)
		{
			fireEvent(point, TEvent::DragEnd);
			_dragMode = false;
		}
		const int32_t deltaX = static_cast<int32_t>(point.x) - _points[0].x;
		const int32_t deltaY = static_cast<int32_t>(point.y) - _points[0].y;
		if (!_tapFiredOnPress && abs(deltaX) <= 15 && abs(deltaY) <= 15 &&
			_touchEndTime - _touchStartTime <= 700)
		{
			fireEvent(point, TEvent::Tap);
			_points[0] = {0, 0};
			_touchStartTime = 0;
		}
		_touchActive = false;
		_pointIdx = 0;
		_tapFiredOnPress = false;
	}
	else
	{
	}
}

void FT6X36::onInterrupt()
{
	if (_isrCounter < 0xFF)
		_isrCounter++;

	if (_isrHandler)
	{
		_isrHandler();
	}
}

bool FT6X36::readData(void)
{
	const uint8_t size = 16;
	uint8_t data[size] = {};
	_wire->beginTransmission(FT6X36_ADDR);
	_wire->write(0);
	if (_wire->endTransmission(false) != 0)
		return false;

	if (_wire->requestFrom((uint8_t)FT6X36_ADDR, size, (uint8_t)true) != size)
		return false;
	for (uint8_t i = 0; i < size; i++)
		data[i] = _wire->read();

#ifdef FT6X36_DEBUG
	Serial.println("REGISTERS:");
	for (int16_t i = 0; i < size; i++)
	{
		Serial.print("0x");
		Serial.print(i, HEX);
		Serial.print(" = 0x");
		Serial.println(data[i], HEX);
	}

	Serial.println();
	Serial.print("TOUCHES: ");
	Serial.println(data[FT6X36_REG_NUM_TOUCHES]);
	Serial.print("GESTURE: ");
	Serial.println(data[FT6X36_REG_GESTURE_ID]);
#endif
	_touches = data[FT6X36_REG_NUM_TOUCHES] & 0x0F;

	const uint8_t addrShift = 6;
	for (uint8_t i = 0; i < 2; i++)
	{
		_touchX[i] = data[FT6X36_REG_P1_XH + i * addrShift] & 0x0F;
		_touchX[i] <<= 8;
		_touchX[i] |= data[FT6X36_REG_P1_XL + i * addrShift];
		_touchY[i] = data[FT6X36_REG_P1_YH + i * addrShift] & 0x0F;
		_touchY[i] <<= 8;
		_touchY[i] |= data[FT6X36_REG_P1_YL + i * addrShift];
		_touchEvent[i] = data[FT6X36_REG_P1_XH + i * addrShift] >> 6;
	}
	return true;
}

bool FT6X36::writeRegister8(uint8_t reg, uint8_t value)
{
	_wire->beginTransmission(FT6X36_ADDR);
	_wire->write(reg);
	_wire->write(value);
	return _wire->endTransmission() == 0;
}

uint8_t FT6X36::readRegister8(uint8_t reg)
{
	_wire->beginTransmission(FT6X36_ADDR);
	_wire->write(reg);
	if (_wire->endTransmission(false) != 0)
		return 0xFF;

	if (_wire->requestFrom((uint8_t)FT6X36_ADDR, (uint8_t)1, (uint8_t)true) != 1)
		return 0xFF;
	uint8_t value = _wire->read();

#ifdef I2C_DEBUG
	Serial.print("REG 0x");
	Serial.print(reg, HEX);
	Serial.print(": 0x");
	Serial.println(value, HEX);
#endif

	return value;
}

void FT6X36::fireEvent(TPoint point, TEvent e)
{
	if (_touchHandler)
		_touchHandler(point, e);
}

#ifdef FT6X36_DEBUG
void FT6X36::debugInfo()
{
	Serial.print("TH_DIFF: ");
	Serial.println(readRegister8(FT6X36_REG_FILTER_COEF));
	Serial.print("CTRL: ");
	Serial.println(readRegister8(FT6X36_REG_CTRL));
	Serial.print("TIMEENTERMONITOR: ");
	Serial.println(readRegister8(FT6X36_REG_TIME_ENTER_MONITOR));
	Serial.print("PERIODACTIVE: ");
	Serial.println(readRegister8(FT6X36_REG_TOUCHRATE_ACTIVE));
	Serial.print("PERIODMONITOR: ");
	Serial.println(readRegister8(FT6X36_REG_TOUCHRATE_MONITOR));
	Serial.print("RADIAN_VALUE: ");
	Serial.println(readRegister8(FT6X36_REG_RADIAN_VALUE));
	Serial.print("OFFSET_LEFT_RIGHT: ");
	Serial.println(readRegister8(FT6X36_REG_OFFSET_LEFT_RIGHT));
	Serial.print("OFFSET_UP_DOWN: ");
	Serial.println(readRegister8(FT6X36_REG_OFFSET_UP_DOWN));
	Serial.print("DISTANCE_LEFT_RIGHT: ");
	Serial.println(readRegister8(FT6X36_REG_DISTANCE_LEFT_RIGHT));
	Serial.print("DISTANCE_UP_DOWN: ");
	Serial.println(readRegister8(FT6X36_REG_DISTANCE_UP_DOWN));
	Serial.print("DISTANCE_ZOOM: ");
	Serial.println(readRegister8(FT6X36_REG_DISTANCE_ZOOM));
	Serial.print("CIPHER: ");
	Serial.println(readRegister8(FT6X36_REG_CHIPID));
	Serial.print("G_MODE: ");
	Serial.println(readRegister8(FT6X36_REG_INTERRUPT_MODE));
	Serial.print("PWR_MODE: ");
	Serial.println(readRegister8(FT6X36_REG_POWER_MODE));
	Serial.print("FIRMID: ");
	Serial.println(readRegister8(FT6X36_REG_FIRMWARE_VERSION));
	Serial.print("FOCALTECH_ID: ");
	Serial.println(readRegister8(FT6X36_REG_PANEL_ID));
	Serial.print("STATE: ");
	Serial.println(readRegister8(FT6X36_REG_STATE));
}
#endif
