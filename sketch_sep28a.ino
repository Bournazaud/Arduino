// Code principal

// =====================================================================
//  Ambulance / BPM  +  Node-RED (MQTT)
//  Matériel : Arduino UNO R4 WiFi
//  Bibliothèque requise : "PubSubClient" (Nick O'Leary)
// =====================================================================

#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Arduino.h>
#include <U8g2lib.h>
#include <PubSubClient.h>

// Gestion automatique de la bibliothèque WiFi selon la carte
#if defined(ARDUINO_UNOR4_WIFI)
  #include <WiFiS3.h>
#elif defined(ESP32)
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFiNINA.h>
#endif

// ======================= CONFIGURATION RÉSEAU =======================
const char* WIFI_SSID = "TEASAPI";
const char* WIFI_PASS = "remy0605@";
#define PREFIXE "LR-ambulance"
const char* MQTT_HOST = "broker.emqx.io";
const int   MQTT_PORT = 1883;
// ====================================================================

// Topics MQTT (Arduino -> Node-RED)
#define T_BPM       PREFIXE "/bpm"
#define T_ETAT      PREFIXE "/etat"
#define T_ENLIGNE   PREFIXE "/en_ligne"

// Topics MQTT (Node-RED -> Arduino)
#define T_MODE      PREFIXE "/mode"
#define T_BOUTON1   PREFIXE "/bouton1"
#define T_REC       PREFIXE "/rec"

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Écran OLED SSD1306 (I2C natif)
U8G2_SSD1306_128X64_ALT0_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// Broches RFID RC522 (Bus SPI)
#define RST_PIN 9
#define SS_PIN 10
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Broches périphériques
const int buttonPin = 2; // Bouton sirène ambulance
const int button2   = 4; // Bouton bascule écran/mode
const int tonepin   = 3; // Buzzer
const int ledR      = 5; // LED Rouge
const int ledV      = 6; // LED Verte (badge)
const int ledPin    = 7; // LED Bleue

// États et modes
int buttonState = 0;
int mode = 1;            // 0 = Ambulance, 1 = BPM
int ancienetat = HIGH;
bool enregistrementActif = false;

// Variables capteur BPM
unsigned char c = 0;
unsigned long dernierBpmTemps = 0;

// Variables temporelles
unsigned long dernierScanTemps = 0;
unsigned long dernierBipTemps = 0;
const unsigned long DUREE_PULSE = 60;

// Variables de synchronisation Node-RED
bool sireneWeb = false;
bool etatModifie = true;
unsigned int badgeCount = 0;

// ---------------------------------------------------------------------
// Fonctions MQTT
// ---------------------------------------------------------------------
void publierEtat() {
  char buf[128];
  bool sireneActive = (mode == 0) && (buttonState == HIGH || sireneWeb);
  snprintf(buf, sizeof(buf),
           "{\"mode\":\"%s\",\"rec\":%s,\"siren\":%s,\"badge\":%u}",
           (mode == 0) ? "ambulance" : "bpm",
           enregistrementActif ? "true" : "false",
           sireneActive ? "true" : "false",
           badgeCount);
  mqtt.publish(T_ETAT, buf, true);
}

void publierBpm(unsigned char valeur) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)valeur);
  mqtt.publish(T_BPM, buf);
}

void surMessage(char* topic, byte* payload, unsigned int len) {
  char msg[24];
  if (len > sizeof(msg) - 1) len = sizeof(msg) - 1;
  memcpy(msg, payload, len);
  msg[len] = 0;

  if (strcmp(topic, T_MODE) == 0) {
    mode = (strcmp(msg, "ambulance") == 0) ? 0 : 1;
    noTone(tonepin);
    digitalWrite(ledPin, LOW);
    digitalWrite(ledR, LOW);
    etatModifie = true;
  } else if (strcmp(topic, T_BOUTON1) == 0) {
    sireneWeb = (strcmp(msg, "on") == 0);
    etatModifie = true;
  } else if (strcmp(topic, T_REC) == 0) {
    enregistrementActif = (strcmp(msg, "on") == 0);
    if (!enregistrementActif) {
      c = 0;
      noTone(tonepin);
      digitalWrite(ledPin, LOW);
      digitalWrite(ledR, LOW);
    }
    etatModifie = true;
  }
}

// Reconnexion asynchrone non-bloquante avec rafraîchissement périodique
void gererReseau() {
  static unsigned long dernierEssai = 0;
  static unsigned long dernierPingEnLigne = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - dernierEssai > 10000) {
      dernierEssai = millis();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }

  if (!mqtt.connected()) {
    if (millis() - dernierEssai > 5000) {
      dernierEssai = millis();
      String id = "LR-amb-" + String((unsigned long)random(0xFFFF), HEX);
      if (mqtt.connect(id.c_str(), NULL, NULL, T_ENLIGNE, 0, true, "off")) {
        mqtt.subscribe(T_MODE);
        mqtt.subscribe(T_BOUTON1);
        mqtt.subscribe(T_REC);
        mqtt.publish(T_ENLIGNE, "on", true);
        publierEtat();
      }
    }
    return;
  }

  mqtt.loop();

  // Rappel régulier toutes les 3 secondes pour forcer Node-RED à rester "En ligne"
  if (millis() - dernierPingEnLigne >= 3000) {
    dernierPingEnLigne = millis();
    mqtt.publish(T_ENLIGNE, "on", true);
    publierEtat();
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000);

  pinMode(ledPin, OUTPUT);
  pinMode(ledR, OUTPUT);
  pinMode(ledV, OUTPUT);
  pinMode(tonepin, OUTPUT);
  pinMode(buttonPin, INPUT);
  pinMode(button2, INPUT_PULLUP);

  SPI.begin();
  Wire.begin();
  mfrc522.PCD_Init();
  u8g2.begin();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(surMessage);

  Serial.println("Systeme pret. Scannez un badge pour demarrer l'enregistrement...");
}

void loop() {
  gererReseau();

  // 1. DÉTECTION DU SCANNER RFID
  if (millis() - dernierScanTemps > 1500) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      dernierScanTemps = millis();
      enregistrementActif = !enregistrementActif;
      badgeCount++;
      etatModifie = true;

      digitalWrite(ledV, HIGH);

      if (enregistrementActif) {
        Serial.println("\n>>> [BADGE ACCEPTE] : DEMARRAGE BPM <<<");
      } else {
        Serial.println("\n>>> [BADGE ACCEPTE] : ARRET BPM (TERMINE) <<<");
        c = 0;
        noTone(tonepin);
        digitalWrite(ledPin, LOW);
        digitalWrite(ledR, LOW);
      }

      delay(800);
      digitalWrite(ledV, LOW);

      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }
  }

  // 2. LECTURE CONTINUE DU CAPTEUR BPM
  if (enregistrementActif) {
    if (millis() - dernierBpmTemps >= 500) {
      dernierBpmTemps = millis();

      Wire.requestFrom(0xA0 >> 1, 1);
      if (Wire.available()) {
        unsigned char lecture = Wire.read();

        if (lecture < 45 || lecture > 200) {
          c = 0;
          Serial.println("[Capteur] Pas de rythme detecte (BPM = 0)");
        } else {
          c = lecture;
          Serial.print("[Capteur actif] BPM : ");
          Serial.println(c, DEC);
        }

        if (mqtt.connected()) {
          publierBpm(c);
        }
      }
    }
  }

  // 3. BOUTON 2 : BASCULE DE L'AFFICHAGE
  int etatbutton2 = digitalRead(button2);
  if (ancienetat == HIGH && etatbutton2 == LOW) {
    mode = !mode;
    noTone(tonepin);
    digitalWrite(ledPin, LOW);
    digitalWrite(ledR, LOW);
    etatModifie = true;

    if (mode == 0) {
      Serial.println(">>> Affichage : Mode Passage Prioritaire");
    } else {
      Serial.println(">>> Affichage : Mode Suivi BPM");
    }
    delay(200);
  }
  ancienetat = etatbutton2;

  // 4. GESTION DES MODES
  buttonState = digitalRead(buttonPin);
  bool declenchementSirene = (mode == 0) && (buttonState == HIGH || sireneWeb);

  if (mode == 0) {
    // Mode Ambulance
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(0, 20);
    u8g2.print("Passage Prioritaire");
    u8g2.sendBuffer();

    if (declenchementSirene) {
      tone(tonepin, 587);
      digitalWrite(ledPin, HIGH);
      digitalWrite(ledR, LOW);
      delay(300);

      tone(tonepin, 440);
      digitalWrite(ledPin, LOW);
      digitalWrite(ledR, HIGH);
      delay(300);
    } else {
      noTone(tonepin);
      digitalWrite(ledPin, LOW);
      digitalWrite(ledR, LOW);
    }

  } else {
    // Mode BPM
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);

    if (!enregistrementActif) {
      u8g2.setCursor(0, 20);
      u8g2.print("En attente badge...");
      u8g2.sendBuffer();

      noTone(tonepin);
      digitalWrite(ledPin, LOW);
      digitalWrite(ledR, LOW);
    } else {

      if (c == 0) {
        u8g2.setCursor(0, 20);
        u8g2.print("ARRET CARDIAQUE");
        u8g2.sendBuffer();

        tone(tonepin, 300);
        digitalWrite(ledR, HIGH);
        digitalWrite(ledPin, LOW);

      } else {
        u8g2.setCursor(0, 20);
        u8g2.print("BPM : ");
        u8g2.print(c);
        u8g2.sendBuffer();

        noTone(tonepin);
        unsigned long intervalleBattement = 60000 / c;

        if (millis() - dernierBipTemps >= intervalleBattement) {
          dernierBipTemps = millis();
          tone(tonepin, 500, DUREE_PULSE);

          if (c > 80 || c < 50) {
            digitalWrite(ledR, HIGH);
            digitalWrite(ledPin, LOW);
          } else {
            digitalWrite(ledPin, HIGH);
            digitalWrite(ledR, LOW);
          }
        }

        if (millis() - dernierBipTemps >= DUREE_PULSE) {
          digitalWrite(ledPin, LOW);
          digitalWrite(ledR, LOW);
        }
      }
    }
  }

  // Publication immédiate en cas de changement d'état
  if (etatModifie && mqtt.connected()) {
    publierEtat();
    etatModifie = false;
  }

  delay(20);
}


// Test des LED
/*
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
}
void loop() {
  digitalWrite(LED_BUILTIN, HIGH);
  delay(1000);
  digitalWrite(LED_BUILTIN, LOW);
  delay(1000);                      
}*/


/*

Test du capteur de tension

#include <Wire.h>
void setup() {
    Serial.begin(9600);
    Serial.println("heart rate sensor:");
    Wire.begin();
}
void loop() {
    Wire.requestFrom(0xA0 >> 1, 1);    // Demande 1 octect au capteur
    while(Wire.available()) {          // Tant que les données sont disponibles envoie de celle-ci
        unsigned char c = Wire.read();   // recupère la valeur du rythme cardiaque (en octet)
        Serial.println(c, DEC);         // Affichage de la valeur du rythme cardiaque en decimal (base 10)
    }
    delay(500);
}*/


// Test du Buzzer
/*
#include <Arduino.h>
int tonepin = 3;

// Fréquences en Hertz (sons graves/moyens, pas stridents)
int tune[] = {262, 330, 392, 523}; // Do, Mi, Sol, Do aigu

// Durées relatives de chaque note
float durt[] = {0.5, 0.5, 0.5, 1.0}; 

int length;

void setup() {
  pinMode(tonepin, OUTPUT);
  length = sizeof(tune) / sizeof(tune[0]);
}

void loop() {
  for (int x = 0; x < length; x++) {
    tone(tonepin, tune[x]);
    delay(200 * durt[x]); // Durée réduite pour que ce soit court
    noTone(tonepin);
    delay(50);            // Petit silence entre les notes
  }

  delay(4000); // Pause de 4 secondes avant de rejouer
}*/


// Test du scanner
/*
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
// Affectation des broches
#define RST_PIN 9
#define SS_PIN 10
MFRC522 mfrc522(SS_PIN, RST_PIN);
const int ledV = 6;

bool enregistrementActif =  false;
unsigned long dernierScanTemps = 0;
unsigned long dernierBpmTemps = 0;

void setup() {
 // Initialisation du Module RFID
 Serial.begin(9600);
 while (!Serial);

 pinMode(ledV, OUTPUT);
 digitalWrite(ledV, LOW);

 SPI.begin();
 Wire.begin();

 mfrc522.PCD_Init(); // Affichage des données de la bibliothèque
 Serial.println("Systeme pret. Scannez un badge pour démarer...");
}
void loop() {
 if (millis() - dernierScanTemps > 1500){
  if(mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()){
    dernierScanTemps = millis();
    enregistrementActif = !enregistrementActif;
    digitalWrite(ledV, HIGH);

    if (enregistrementActif){
      Serial.println("\n>>> [BADGE ACCEPTE] : Demarage de l'enregistement BPM <<<");
    }else{
      Serial.println("\n>>> [BADGE ACCEPTE] : ARRET DE L'ENREGISTREMENT (TERMINE) <<<");
    }
    delay(1000);
    digitalWrite(ledV, LOW);

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
 }
 if (enregistrementActif){
  if (millis() - dernierBpmTemps >= 500){
    dernierBpmTemps = millis();

    Wire.requestFrom(0xA0 >>1,1);
    if(Wire.available()){
      unsigned char bpm = Wire.read();

      if (bpm < 45 || bpm > 200) {
        Serial.println("Calibrage en cours... Restez bien immobile !");
      } else {
        Serial.print("Enregistrement en cours... BPM : ");
        Serial.println(bpm, DEC);
      }
    }
  }
 }
}
*/







