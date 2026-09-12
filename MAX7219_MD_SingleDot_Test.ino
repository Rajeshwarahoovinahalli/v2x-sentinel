
#include <MD_MAX72xx.h>
#include <SPI.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 1

#define CLK_PIN   10
#define DATA_PIN  12
#define CS_PIN    11

MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

void setup() {
  mx.begin();
  mx.clear();

  mx.setPoint(0, 0, true);
}

void loop() {

}
