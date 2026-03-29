#include "hcs301_receiver.h"
#include <Arduino.h>

// Implementacja dekodera HCS301 pracującego w przerwaniach
// Dekodowanie odbywa się w ISR i ustawia flage gotowości. Wywołanie callbacku
// odbywa się w pętli `loop()` aby uniknąć wywoływania ciężkich operacji w ISR.

// --- zmienne używane przez ISR (volatile) ---
static volatile bool HCS_listening = true;
static volatile uint32_t HCS_last_change = 0;
static volatile uint8_t HCS_bit_array[66];
static volatile uint8_t HCS_bit_counter = 0;
static volatile uint8_t HCS_preamble_count = 0;
static volatile bool bPreamble = false;
static volatile bool bHeader = false;
static volatile bool bInv = false;

// Zmienne do debugowania (konsumowane w loop())
static volatile bool hcs_debug_ready = false;
static volatile uint32_t hcs_debug_serial = 0;
static volatile uint32_t hcs_debug_rolling = 0;
static volatile uint8_t hcs_debug_bits[10]; // Pierwsze 10 bitów do debugu

// Zdekodowane dane - ustawiane w ISR i konsumpowane w loop()
struct DecodedMsg {
  volatile uint32_t SerialNum;
  volatile uint32_t Encript;
  volatile bool BtnToggle;
  volatile bool BtnGreen;
  volatile bool BatteryLow;
  volatile bool Ready;
};
static DecodedMsg decoded = {0,0,false,false,false,false};
static volatile int hcs_pin = -1; // pin ustawiany w begin()
static volatile bool learnModeFlag = false; // tryb nauki

// Forward declaration ISR
void IRAM_ATTR HCS_ISR();

void HCS301Receiver::setLearnMode(bool enable) {
  learnModeFlag = enable;
}

bool HCS301Receiver::isLearnMode() const {
  return learnModeFlag;
}

HCS301Receiver::HCS301Receiver(int pin_) : pin(pin_), cb(nullptr), last_change(0) {}

void HCS301Receiver::begin() {
  pinMode(pin, INPUT);
  hcs_pin = pin;
  // Attach interrupt (CHANGE) to decode pulses
  attachInterrupt(digitalPinToInterrupt(pin), HCS_ISR, CHANGE);
  Serial.println("[HCS] ISR zainicjalizowany na pin " + String(pin));
}

void HCS301Receiver::loop() {
  // Wyświetl debug info jeśli dostępne
  if (hcs_debug_ready) {
    Serial.print("[HCS] BtnBits: ");
    Serial.print(hcs_debug_bits[0]); // Repeat
    Serial.print(hcs_debug_bits[1]); // BatteryLow  
    Serial.print(hcs_debug_bits[2]); // BtnNoSound
    Serial.print(hcs_debug_bits[3]); // BtnOpen/Toggle
    Serial.print(hcs_debug_bits[4]); // BtnClose/Green
    Serial.print(hcs_debug_bits[5]); // BtnRing
    Serial.println();
    hcs_debug_ready = false;
  }
  
  // Jeśli jest gotowa zdekodowana wiadomość i jest przypisany callback, wywołaj go
  if (decoded.Ready && cb) {
    noInterrupts(); // bezpieczne skopiowanie volatile danych
    uint32_t serial = decoded.SerialNum;
    uint32_t encript = decoded.Encript;
    bool t = decoded.BtnToggle;
    bool g = decoded.BtnGreen;
    bool b = decoded.BatteryLow;
    decoded.Ready = false; // konsumujemy
    HCS_listening = true; // ponownie włączamy nasłuchiwanie
    interrupts();

    Serial.print("[HCS] Loop callback: Serial=");
    Serial.print(serial);
    Serial.print(" Encript=");
    Serial.print(encript);
    Serial.print(" BtnToggle=");
    Serial.print(t);
    Serial.print(" BtnGreen=");
    Serial.print(g);
    Serial.print(" BatteryLow=");
    Serial.println(b);

    // Wywołaj callback poza sekcją krytyczną
    cb(serial, encript, t, g, b);
  }
}

void HCS301Receiver::setCallback(HCSCallback cb_) {
  cb = cb_;
  Serial.println("[HCS] Callback ustawiony");
}

// ----------------------------------------------------------------------------
// ISR: prosty dekoder HCS301 oparty na czasach trwania impulsów (micros())
// Bazuje na implementacji wcześniejszej w projekcie; zoptymalizowane do pracy
// w ISR (minimalne operacje) i przekazuje wynik przez `decoded`.
// ----------------------------------------------------------------------------

void IRAM_ATTR HCS_ISR() {
  if (!HCS_listening) return;
  
  uint32_t cur = micros();
  uint8_t cur_status = digitalRead(hcs_pin);
  uint32_t pulse = cur - HCS_last_change;
  HCS_last_change = cur;

  // Szukamy preambuły: krótkie impulsy powtarzane
  if (bPreamble) {
    if (pulse > 3000 && pulse < 6000) {  // Długi impuls = header
      bHeader = true;
      HCS_bit_counter = 0;
      bPreamble = false;
      bInv = (cur_status == LOW);
    } else if (pulse < 300 || pulse > 600) {
      // zły preambuła
      bPreamble = false;
    }
    return;
  }

  if (bHeader) {
    if ((pulse > 300) && (pulse < 1100)) {
      if (cur_status == (bInv ? HIGH : LOW)) {
        if (HCS_bit_counter < 66) {
          // Zapis w odwrotnej kolejności - jak w Twojej implementacji
          HCS_bit_array[65 - HCS_bit_counter] = (pulse < 600) ? 1 : 0;
          HCS_bit_counter++;
          
          if (HCS_bit_counter == 66) {
            // --- DEKODOWANIE zgodnie z TWOJĄ starą implementacją ---
            
            // W TWOJEJ starej implementacji struktura jest:
            // Bit 0: Repeat
            // Bit 1: BattaryLow
            // Bit 2: BtnNoSound
            // Bit 3: BtnOpen
            // Bit 4: BtnClose
            // Bit 5: BtnRing
            // Bit 6-33: SerialNum (28 bitów)
            // Bit 34-65: Encript (32 bity)
            
            bool repeat = HCS_bit_array[0];
            bool battery_low = HCS_bit_array[1];
            bool btn_no_sound = HCS_bit_array[2];
            bool btn_open = HCS_bit_array[3];
            bool btn_close = HCS_bit_array[4];
            bool btn_ring = HCS_bit_array[5];
            
            // Numer seryjny (28 bitów): bity 6-33
            uint32_t serial = 0;
            for (int i = 6; i < 34; i++) {
              serial = (serial << 1) | HCS_bit_array[i];
            }
            
            // Kod narastający (32 bity): bity 34-65
            uint32_t rolling = 0;
            for (int i = 34; i < 66; i++) {
              rolling = (rolling << 1) | HCS_bit_array[i];
            }
            
            // Zapisujemy dane - używamy przycisków jak w Twojej implementacji
            decoded.SerialNum = serial;
            decoded.Encript = rolling;
            decoded.BatteryLow = battery_low;
            decoded.BtnToggle = btn_open;     // btn_open -> BtnToggle
            decoded.BtnGreen = btn_close;     // btn_close -> BtnGreen
            decoded.Ready = true;

            // Ustaw dane debugowe (do wyświetlenia w loop)
            hcs_debug_serial = serial;
            hcs_debug_rolling = rolling;
            for (int i = 0; i < 10; i++) {
              hcs_debug_bits[i] = HCS_bit_array[i];
            }
            hcs_debug_ready = true;

            // Wyłącz nasłuchiwanie
            HCS_listening = false;
            bHeader = false;
            HCS_preamble_count = 0;
          }
        }
      }
    } else {
      // błąd w header
      bHeader = false;
    }
    return;
  }

  // Jeżeli nie preambuła i nie header -> szukaj preambuły
  if (pulse > 300 && pulse < 600) {
    if (++HCS_preamble_count > 10) {
      bPreamble = true;
      HCS_preamble_count = 0;
    }
  } else {
    HCS_preamble_count = 0;
  }
}