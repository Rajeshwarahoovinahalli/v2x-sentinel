
#include <MD_MAX72xx.h>
#include <SPI.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 1

#define CLK_PIN   10
#define DATA_PIN  12
#define CS_PIN    11

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

byte letter_M[8] = {
  B11111111,
  B01000000,
  B00100000,
  B00010000,
  B00100000,
  B01000000,
  B11111111,
  B00000000
};

void setup() {
  mx.begin();
  mx.clear();

  for (int col = 0; col < 8; col++) {
    mx.setColumn(col, letter_M[col]);
  }
}

void loop() {

}
