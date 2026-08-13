// ─────────────────────────────────────────────────────────────────────────
//  01_bringup_i2c — I2C bus diagnostic for the Cadence hub
//  Target : ESP32-S3 DevKit N8R2.  SDA = GPIO 8, SCL = GPIO 9.
//
//  A silent I2C part raises three questions, and they have to be answered in
//  this order or you end up re-soldering a chip that was never powered:
//
//    1. Is the module actually powered?   — pull-up presence probe
//    2. Is anything on the bus at all?    — scan, at three speeds
//    3. Which Bosch part is this?         — chip ID at register 0xD0
//
//  Re-runs every 5 s, so you can wiggle a joint and watch the verdict change.
//  No libraries beyond Wire.
// ─────────────────────────────────────────────────────────────────────────

#include <Wire.h>

#define PIN_SDA 8
#define PIN_SCL 9

#define ADDR_BME     0x76
#define ADDR_BME_ALT 0x77
#define ADDR_BH1750  0x23

static const uint32_t SPEEDS[] = { 100000, 50000, 400000 };
static const int      SPEED_COUNT = sizeof(SPEEDS) / sizeof(SPEEDS[0]);

// ── 1. power, without a multimeter ───────────────────────────────────────
//
// Both I2C modules carry their own pull-up resistors tied to their VCC. Drive
// the line down through the ESP32's internal pulldown and see who wins: a
// powered external pull-up holds it HIGH, an unpowered one cannot, so the
// pulldown wins and the line reads LOW.
//
// This is the trick that proves a module's rail is live with nothing but the
// devkit. It says nothing about the chip — only that its VCC pin has voltage.
static bool pullupHoldsHigh(uint8_t pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delayMicroseconds(500);
  bool high = digitalRead(pin) == HIGH;
  pinMode(pin, INPUT);
  return high;
}

// ── 2. who is on the bus ─────────────────────────────────────────────────
static int scanBus(uint32_t hz, uint8_t *found, int maxFound) {
  Wire.end();
  Wire.begin(PIN_SDA, PIN_SCL, hz);
  delay(20);

  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && n < maxFound) found[n++] = a;
  }
  return n;
}

// ── 3. which chip ────────────────────────────────────────────────────────
static int readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;   // repeated start
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

static const char *boschPart(int id) {
  switch (id) {
    case 0x60: return "BME280  — correct part, has humidity";
    case 0x58: return "BMP280  — NO HUMIDITY SENSOR, wrong part";
    case 0x61: return "BME680  — works, but needs a different library";
    case 0x56:
    case 0x57: return "BMP280 sample silicon — wrong part";
    default:   return "unknown — not a Bosch part at this address?";
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n\n=== Cadence I2C bring-up ===");
  Serial.printf("SDA = GPIO %d, SCL = GPIO %d\n", PIN_SDA, PIN_SCL);
}

void loop() {
  Serial.println("\n────────────────────────────────────────────");

  // 1. power
  Wire.end();
  delay(5);
  bool sdaUp = pullupHoldsHigh(PIN_SDA);
  bool sclUp = pullupHoldsHigh(PIN_SCL);

  Serial.printf("[1] pull-ups   SDA %s   SCL %s\n",
                sdaUp ? "HIGH (powered)" : "LOW  (NO POWER)",
                sclUp ? "HIGH (powered)" : "LOW  (NO POWER)");
  if (!sdaUp || !sclUp) {
    Serial.println("    -> No powered pull-up on at least one line. Check VIN/3V3");
    Serial.println("       and GND on the modules before suspecting the chip.");
  }

  // 2. scan
  uint8_t found[16];
  int total = 0;
  for (int s = 0; s < SPEED_COUNT; s++) {
    int n = scanBus(SPEEDS[s], found, 16);
    Serial.printf("[2] scan @ %6lu Hz : ", (unsigned long)SPEEDS[s]);
    if (n == 0) {
      Serial.println("nothing responded");
    } else {
      for (int i = 0; i < n; i++) {
        Serial.printf("0x%02X ", found[i]);
        if (found[i] == ADDR_BH1750)                                 Serial.print("(BH1750) ");
        if (found[i] == ADDR_BME || found[i] == ADDR_BME_ALT)        Serial.print("(Bosch) ");
      }
      Serial.println();
    }
    total += n;
  }

  // 3. chip id — back to the speed the hub firmware uses
  Wire.end();
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  delay(20);

  bool sawBosch = false;
  for (uint8_t a = ADDR_BME; a <= ADDR_BME_ALT; a++) {
    int id = readReg(a, 0xD0);
    if (id >= 0) {
      sawBosch = true;
      Serial.printf("[3] 0x%02X reg 0xD0 = 0x%02X   %s\n", a, id, boschPart(id));
    }
  }
  if (!sawBosch) {
    Serial.println("[3] no Bosch sensor answered at 0x76 or 0x77.");

    // A swapped SDA/SCL pair is silent in exactly the same way as a dead
    // chip, and it is the one wiring mistake this sketch can test for itself.
    Wire.end();
    Wire.begin(PIN_SCL, PIN_SDA, 100000);   // deliberately reversed
    delay(20);
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) n++;
    }
    if (n > 0) {
      Serial.println("    -> !! Devices DO respond with SDA and SCL reversed.");
      Serial.println("       The two wires are swapped. Fix the wiring, not the chip.");
    }
    Wire.end();
    Wire.begin(PIN_SDA, PIN_SCL, 100000);
  }

  // verdict
  Serial.print("[=] ");
  if (sawBosch) {
    Serial.println("Bosch sensor is present. Check the chip ID line above.");
  } else if (!sdaUp || !sclUp) {
    Serial.println("No powered pull-up on the bus — this is a power/ground "
                   "fault, not a dead chip. Fix VIN/GND first.");
  } else if (total == 0) {
    // Nothing at all answered, yet something is holding the lines up. With
    // only one module attached those pull-ups are its own, so it has power
    // and still will not talk: that is a dead part, not a wiring fault.
    Serial.println("Pull-ups are powered but NOTHING answers at any speed. If "
                   "this module is the only one on the bus, it has power and "
                   "is still silent — the chip is dead.");
  } else {
    Serial.println("Bus is powered and other devices answer, but the Bosch "
                   "part is silent. Power and wiring are good; the sensor "
                   "is not.");
  }

  delay(5000);
}
