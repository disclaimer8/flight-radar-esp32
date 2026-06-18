#pragma once
#include <Arduino.h>
#include <Wire.h>

#define CST816S_ADDR     0x15
#define CST816S_REG_GEST 0x01

// CST816S gesture register values.
enum TouchGesture {
    TG_NONE   = 0x00,
    TG_UP     = 0x01,
    TG_DOWN   = 0x02,
    TG_LEFT   = 0x03,
    TG_RIGHT  = 0x04,
    TG_CLICK  = 0x05,
    TG_DOUBLE = 0x0B,
    TG_LONG   = 0x0C,
};

class CST816S {
public:
    CST816S(int sda, int scl, int rst, int intp)
        : _sda(sda), _scl(scl), _rst(rst), _int(intp) {}

    void begin() {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, LOW);  delay(10);
        digitalWrite(_rst, HIGH); delay(50);
        pinMode(_int, INPUT_PULLUP);
        Wire.begin(_sda, _scl);
        Wire.setClock(400000);   // CST816S supports 400 kHz fast-mode
        Wire.setTimeOut(10);     // ms: a stuck SDA can't hang the render loop
    }

    // Returns the current gesture register value (TG_* ), or TG_NONE on I2C error.
    uint8_t readGesture() {
        Wire.beginTransmission(CST816S_ADDR);
        Wire.write(CST816S_REG_GEST);
        if (Wire.endTransmission(false) != 0) return TG_NONE;
        if (Wire.requestFrom(CST816S_ADDR, 1) != 1) return TG_NONE;
        return Wire.read();
    }

private:
    int _sda, _scl, _rst, _int;
};
