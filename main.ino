
#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <SPI.h>

#include "nrf24.h"
#include "ppm.h"
#include "telemetry.h"

static byte reportBuffer[8];
unsigned int hover_id;

#define DEBUG

// classic RETA channel mapping
#define CH_YAW 0
#define CH_PITCH 1
#define CH_THRUST 2
#define CH_ROLL 3
// channels 5-7: use for hover, arm/disarm and x-mode
#define CH_HOVER 4
#define CH_ARMED 5
#define CH_XMODE 6
#define CH_ALTHOLD_CHANGE 2

// -------------------------------------------------------------------------

void setup() {
  int i;
  NRF24_RADIO_t radio;

  pinMode(8, INPUT); // ppm input
  pinMode(LED_PIN, OUTPUT);

  Serial.begin(9600); // 
  Serial.println("\r\n\r\nAli's PPM to Crazyflie module. Wooohoo1!\r\n");
  Serial.println(F_CPU);

  // Turn link LED off.
  digitalWrite(LED_PIN, 0);


  // Initialize SPI device associated with NRF24L01 and attach it to the bus.
  memset(&radio, 0, sizeof(radio));
  radio.variant = NRF24L01P;

  nrf24_begin();

  // Configure fast data rate, high power, 32-byte ACK payload.
  nrf24_set_data_rate(&radio, 1000);
  nrf24_set_transmit_power(&radio, 0);
  nrf24_set_ack_payload_length(&radio, 32);
  nrf24_set_auto_retransmit_count(&radio, 10);

  // Zap any outstanding packets.
  for (i = 0; i < 10; ++i)
    {
      nrf24_flush_tx(&radio);
      nrf24_flush_rx(&radio);
    }

  Serial.println(" done.");

  // Show title and wait for user to read it.
  Serial.println("Scanning... Please wait");

  // Channel scan.
  while (crazyflie_channel == 0) {
    channel_scan(&radio);
    Serial.println("ERROR: Crazyflie Not Detected");
  }

  // Control copter.
  Serial.println("Crazyflie Go!");

  // Turn link LED on.
  digitalWrite(LED_PIN, 1);

  // Send command packets.
  nrf24_set_channel(&radio, crazyflie_channel);

  // find out the TOC id for flightmode.althold
  hover_id = cf_find_hover_param(&radio);

  // set up FrSky compatible vbat logging. There can't be too much Serial output in the main loop for this to work.
  cf_log_setup(&radio);

  wdt_enable(WDTO_1S);
  ppmInit();
  sei();
  ppmNewData=1;
}


void loop() {

  NRF24_RADIO_t radio;
  CRAZYFLIE_COMMANDER_SETPOINT_t setpoint;
  CRAZYFLIE_CONSOLE_t ack;
  unsigned int ack_size;
  int stat;

  bool hover;
  bool armed;
  bool xmode;

  setpoint.c = 3 << 4; // command port
  setpoint.roll = 0;
  setpoint.pitch = 0;
  setpoint.yaw = 0;

  wdt_reset();

  if (ppmNewData) {
    //Serial.println("new PPM Data!");
    ppmNewData=0;
    for (uint8_t i=0;i<sizeof(reportBuffer);i++) {
      unsigned char val=ppmGet(i);
      if (reportBuffer[i]!=val) {
        reportBuffer[i]=val;
      }
    }

    hover = (reportBuffer[CH_HOVER] > 128) ? 1 : 0;
    armed = (reportBuffer[CH_ARMED] > 128) ? 1 : 0;
    xmode = (reportBuffer[CH_XMODE] > 128) ? 1 : 0;

    cf_hovermode(&radio, hover);

    if (hover) {
      // in althold mode the CF expects 32767 to hold its altitude
      setpoint.thrust = reportBuffer[CH_ALTHOLD_CHANGE] * 256 - 1;
    } else {
      // in normel mode MAX_THRUST is clamped to 60k
      setpoint.thrust = reportBuffer[CH_THRUST] * 235;
    }

    setpoint.yaw = (float)(reportBuffer[CH_YAW] - 128) / 128 * 200; // -200.0 - 200.0

    // this is virtually uncontrollable. compensate in mixer settings!
    setpoint.pitch = (float)(reportBuffer[CH_PITCH] - 128) / 128 * 360; // 360 degrees, FUCK YEAH!
    setpoint.roll = (float)(reportBuffer[CH_ROLL] - 128) / 128 * 360; // 

    // arm/disarm
    if (!armed)
      setpoint.thrust = 0;

    // x-mode
    if (!xmode) {
      float roll_copy = setpoint.roll;
      setpoint.roll = 0.707 * (setpoint.roll - setpoint.pitch);
      setpoint.pitch = 0.707 * (roll_copy + setpoint.pitch);
    }

    setpoint.pitch = -setpoint.pitch;
      
  }

  // Send control packet.
  stat = nrf24_send_packet(&radio, &setpoint, sizeof(setpoint), &ack, &ack_size);

  // Reflect status on link LED: on when good link, off when lost.
  digitalWrite(LED_PIN, stat > 0);

  // Process console data coming back.
  if (stat >= 0 && ack_size > 0) {
    if ((ack.c >> 4) == 0) {
      // Console data.
      char *p = strchr((char *)ack.console.data, '\n');
      if (p) {
        p[1] = 0;
      } else{
        ack.console.data[ack_size-1] = 0;
      }
      // build data frames
      //Serial.print("ACK Data: ");
      //Serial.println((char *)ack.console.data);
    } else if (((ack.c >> 4) == 5) && (ack.console.data[0] == 0xbb)) {
      // Logging port.
      static float fbat;
      memcpy(&fbat, &ack.console.data[4], 4);
      uint8_t vbat = (fbat * 1000) * 255 / 4200; // 255 = 4.22V = 4220mV

      frskySendPacket(vbat);
    }
  }
}

