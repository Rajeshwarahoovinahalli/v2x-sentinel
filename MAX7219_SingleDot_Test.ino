#include "LedControl.h"

LedControl lc = LedControl(12, 10, 11, 1);

void setup() {
  lc.shutdown(0, false);
  lc.setIntensity(0, 8);
  lc.clearDisplay(0);

  lc.setLed(0, 0, 0, true);
}

void loop() {

}
