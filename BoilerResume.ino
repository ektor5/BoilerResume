// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BoilerResume
 *
 * Copyright 2024 Ettore Chimenti <ek5.chimenti@gmail.com>
 *
*/

#include <Arduino.h>
#include <ESP32Servo.h> 

#include <Wire.h>
#include <Adafruit_TCS34725.h>

#include <time.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <AsyncTelegram2.h>

#include "BoilerResumeSecrets.h" 

/* Servo */
#define PIN_SERVO 5
#define POSITION_ON 70
#define POSITION_OFF 40
Servo myservo;  // create servo object to control a servo

/* RGB Sensor */
/* Initialise with specific int time and gain values */
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_614MS, TCS34725_GAIN_1X);
const int interruptPin = 0;

volatile boolean state = false;
volatile boolean boilerfail = false;
volatile boolean boilerfail_handled = false;
int retries = 0;

#define MYTZ "CET-1CEST,M3.5.0,M10.5.0/3"

const char* ssid     = MYSSID;     // your network SSID (name of wifi network)
const char* password = MYWIFIPWD; // your network password

WiFiClientSecure client;

// Telegram
AsyncTelegram2 myBot(client);
int64_t userid = MYUSERID;    // Telegram userid
const char* token = MYTOKEN;  // Telegram token
bool debuglight = false;

#define MAX_RETRIES 5

void getRawData_noDelay(uint16_t *r, uint16_t *g, uint16_t *b, uint16_t *c)
{
  *c = tcs.read16(TCS34725_CDATAL);
  *r = tcs.read16(TCS34725_RDATAL);
  *g = tcs.read16(TCS34725_GDATAL);
  *b = tcs.read16(TCS34725_BDATAL);
}

//Interrupt Service Routine
void isr() 
{
  state = true;
}

void setup() {
  //Initialize serial and wait for port to open:
  Serial.begin(115200);
  delay(100);
  
  // servo
	// Allow allocation of all timers
	ESP32PWM::allocateTimer(0);
	ESP32PWM::allocateTimer(1);
	ESP32PWM::allocateTimer(2);
	ESP32PWM::allocateTimer(3);
  myservo.setPeriodHertz(50);// Standard 50hz servo
  myservo.attach(PIN_SERVO, 500, 2400);
  myservo.write(POSITION_OFF);

  // WIFI
  Serial.print("Attempting to connect to SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  // attempt to connect to Wifi network:
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    // wait 1 second for re-trying
    delay(1000);
  }

  // TELEGRAM
  Serial.print("Connected to ");
  Serial.println(ssid);
  configTzTime(MYTZ, "time.google.com", "time.windows.com", "pool.ntp.org");
  client.setCACert(telegram_cert);

  // Set the Telegram bot properties
  myBot.setUpdateTime(1000);
  myBot.setTelegramToken(token);

  // Check if all things are ok
  Serial.print("\nTest Telegram connection... ");
  myBot.begin() ? Serial.println("OK") : Serial.println("NOK");
  Serial.print("Bot name: @");
  Serial.println(myBot.getBotName());
  
  time_t now = time(nullptr);
  struct tm t = *localtime(&now);
  char welcome_msg[64];
  strftime(welcome_msg, sizeof(welcome_msg), "Bot started at %X", &t);
  myBot.sendTo(userid, welcome_msg);

  // TCS
  pinMode(interruptPin, INPUT_PULLUP); //TCS interrupt output is Active-LOW and Open-Drain
  attachInterrupt(digitalPinToInterrupt(interruptPin), isr, FALLING);

  if (tcs.begin()) {
    Serial.println("Found sensor");
  } else {
    Serial.println("No TCS34725 found ... check your connections");
    while (1);
  }
  
  // Set persistence filter to generate an interrupt for every RGB Cycle, regardless of the integration limits
  tcs.write8(TCS34725_PERS, TCS34725_PERS_NONE); 
  tcs.setInterrupt(true);

  myservo.write(POSITION_ON);

  Serial.flush();
}

void boilerfail_handling(void) {
    time_t now = time(nullptr);
    struct tm t = *localtime(&now);
    char msg_buf[64];
    strftime(msg_buf, sizeof(msg_buf), "%X - blocco!!!", &t);
    myBot.sendTo(userid, msg_buf);
}

void boilerfail_solved(void) {
    time_t now = time(nullptr);
    struct tm t = *localtime(&now);
    char msg_buf[32];
    char msg_f_buf[64];
    strftime(msg_buf, sizeof(msg_buf), "%X - risolto", &t);
    sprintf(msg_f_buf, "%s (provato %d volte)!", msg_buf, retries);
    myBot.sendTo(userid, msg_f_buf);
}

void boilerfail_keepfailing(void) {
    time_t now = time(nullptr);
    struct tm t = *localtime(&now);
    char msg_buf[48];
    char msg_f_buf[64];
    strftime(msg_buf, sizeof(msg_buf), "%X - Blocco totale", &t);
    sprintf(msg_f_buf, "%s (provato %d volte)!", msg_buf, retries);
    myBot.sendTo(userid, msg_f_buf);
}

void loop() {
  // In the meantime LED_BUILTIN will blink with a fixed frequency
  // to evaluate async and non-blocking working of library
  static uint32_t ledTime = millis();
  if (millis() - ledTime > 200) {
    ledTime = millis();
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }

  // Check incoming messages and keep Telegram server connection alive
  TBMessage msg;
  if (myBot.getNewMessage(msg)) {    
    Serial.print("User ");
    Serial.print(msg.sender.username);
    Serial.print(" sent this message: ");
    Serial.println(msg.text);    

    if (msg.text == "debuglight") {
      debuglight = true;
      msg.text = "lightdebug activated";
    }

    myBot.sendMessage(msg, msg.text);
  }

  // wait for the rgb sensor to be done with integration time
  if (state) {
    uint16_t r, g, b, c, lux;
    getRawData_noDelay(&r, &g, &b, &c);
    lux = tcs.calculateLux(r, g, b);

    if (debuglight) {
      char text[64];
      sprintf(text, "r %d, g %d, b %d, lux %d", r, g, b, lux);
      myBot.sendTo(userid, text);
          Serial.print("Lux: "); Serial.print(lux, DEC); Serial.print(" - ");
          Serial.print("R: "); Serial.print(r, DEC); Serial.print(" ");
          Serial.print("G: "); Serial.print(g, DEC); Serial.print(" ");
          Serial.print("B: "); Serial.print(b, DEC); Serial.print(" ");
          Serial.print("C: "); Serial.print(c, DEC); Serial.print(" ");
          Serial.println(" ");

      debuglight = false;
    }

    // verify red light is on
    if (r > g && r > b && lux > 1000){
      boilerfail = true;

      if (retries > MAX_RETRIES) {
        boilerfail_keepfailing();
        delay(1000);
        // halt: servo is probably broken 
        while(1) 
          sleep(100);
      }
      
    } else {
      boilerfail = false;

      if (boilerfail_handled) {
        boilerfail_handled = false;
        boilerfail_solved();
        retries = 0;
      }
    }

    tcs.clearInterrupt();
    state = false;
  }

  if (boilerfail && !boilerfail_handled) {
    // halt message
    boilerfail_handling();
    boilerfail_handled = true;
  }

  if (boilerfail_handled){
    // servo action
    myservo.write(POSITION_OFF);
    delay(1000);
    myservo.write(POSITION_ON);
    delay(1000);
    retries++;
  }
}