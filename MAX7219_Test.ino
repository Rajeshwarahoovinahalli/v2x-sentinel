
#include "LedControl.h"

LedControl lc = LedControl(12, 10, 11, 1);

byte letter_M[8] = {
  B10000001,
  B11000011,
  B10100101,
  B10011001,
  B10000001,
  B10000001,
  B10000001,
  B10000001
};

void setup() {

  lc.shutdown(0, false);
  lc.setIntensity(0, 8);
  lc.clearDisplay(0);

  for (int row = 0; row < 8; row++) {
    lc.setRow(0, row, letter_M[row]);
  }
}

void loop() {

  delay(1000);
  lc.setIntensity(0, 2);
  delay(300);
  lc.setIntensity(0, 8);
}
