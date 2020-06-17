#include <Arduino.h>
#include <NativeEthernet.h>
#include <NativeEthernetUdp.h>
#include <ArtNode.h>
#include <ArtNetFrameExtension.h>
#include <TriantaduoWS2811/TDWS2811.h>

///////////////////////////////////////////////////////////////////////////////
// debug mode: 0 no debug, 1 debug
// when in debug mode, some informations a sent via serial
// debug slows down the artnet decoding!!
#define DEBUG 1

///////////////////////////////////////////////////////////////////////////////
// conf: the only things that need to be touched
#define NAME "PASSENGERS 1"
#define LONG_NAME "PASSENGERS 1"
#define CUSTOM_ID 1
// pass sync to 0 if your software don't send art-sync packets (e.g millumin, max/msp, and so on)
#define SYNC 1

///////////////////////////////////////////////////////////////////////////////
// TOUCH THE BELOW CODE AT YOUR OWN RISKS!!
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// output config
#define NUM_OF_OUTPUTS 32
#define MAX_NUM_LED_PER_OUTPUT 300
#define NUM_CHANNEL_PER_LED 4

// buffer size of the OctoWS2811 lib
const int outputSize = MAX_NUM_LED_PER_OUTPUT * 2 * NUM_CHANNEL_PER_LED;
// how many DMX channels per led strip
// e.g.: 600 * 3 = 1800 DMX channels for 600 RGB leds per output
// e.g.: 300 * 4 = 1200 DMX cghannels for 300 RGBW leds per output
const int channelsPerOutput = MAX_NUM_LED_PER_OUTPUT * NUM_CHANNEL_PER_LED;
// how many universes are needed for a strip
// e.g.: 4 universes for 1800 channels
// e.g.: 3 universes for 1200 channels
const int universesPerOutput = (channelsPerOutput % 512) ? channelsPerOutput / 512 + 1 : channelsPerOutput / 512;
// TODO: find a better way!!
// how many led on full universe
const int ledsOnFullUniverse = 512 / NUM_CHANNEL_PER_LED;
// how many leds on the last universe
const int fullUniverses = MAX_NUM_LED_PER_OUTPUT / ledsOnFullUniverse;
const int ledsOnLastUniverse = MAX_NUM_LED_PER_OUTPUT - (fullUniverses * ledsOnFullUniverse);
// how many universes in total
// e.g.: 4 * 8 = 32 for 8 strips of 600 RGB leds
const int numArtnetPorts = universesPerOutput * NUM_OF_OUTPUTS;
// pixels
TDWS2811 td;
color_t white = {255, 255, 255, 255};
color_t black = {0, 0, 0, 0};

///////////////////////////////////////////////////////////////////////////////
// artnet and udp conf
#define VERSION_HI 0
#define VERSION_LO 1
ArtConfig config = {
  {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   // MAC address, will be overwritten with the one burnt into the teensy
  {0, 0, 0, 0},                         // IP, will be generated from the mac adress as artnet spec tells us
  {255, 0, 0, 0},                       // subnet mask
  0x1936,                               // UDP port
  false,                                // DHCP
  0, 0,                                 // net (0-127) and subnet (0-15)
  NAME,                                 // short name
  LONG_NAME,                            // long name
  numArtnetPorts,                       // number of ports
  {                                     // port types
    PortTypeDmx | PortTypeOutput,
    PortTypeDmx | PortTypeOutput,
    PortTypeDmx | PortTypeOutput,
    PortTypeDmx | PortTypeOutput
  },
  {0, 0, 0, 0},                         // port input universes (0-15)
  {0, 1, 2, 3},                         // port output universes (0-15)
  VERSION_HI,
  VERSION_LO
};
const int oemCode = 0x0000; // OemUnkown
ArtNodeExtended node;
EthernetUDP udp;
// 576 is the max size of an artnet packet
byte buffer[576];

///////////////////////////////////////////////////////////////////////////////
void setup() {
  if (DEBUG) Serial.begin(115200);
  // init leds
  pinMode(0, OUTPUT);
  for (int i = 0; i < NUM_OF_OUTPUTS; i++) {
    td.setChannelType(i, GRBW);
  }
  // proceed a blink to check if everything is ok
  blink();
  // generating mac
  teensyMAC(config.mac);
  // generating IP
  config.ip[0] = 2;
  config.ip[1] = 1;
  config.ip[2] = 0;
  config.ip[3] = CUSTOM_ID;
  if (DEBUG) diagnostic();
  IPAddress gateway(config.ip[0], 0, 0, 1);
  IPAddress subnet(255, 0, 0, 0);
  // start ethernet, udp and artnet
  Ethernet.setStackHeap(1024 * 128);
  Ethernet.setSocketSize(4 * 1460);
  Ethernet.setSocketNum(1);
  Serial.println("ETH BEGIN");
  Ethernet.begin(config.mac, config.ip, gateway, gateway, subnet);
  udp.begin(config.udpPort);
  node = ArtNodeExtended(config, sizeof(buffer), buffer);
}

///////////////////////////////////////////////////////////////////////////////
void loop() {
  while (udp.parsePacket()) {
    // read the header to make sure it's Art-Net
    unsigned int n = udp.read(buffer, sizeof(ArtHeader));
    if (n >= sizeof(ArtHeader)) {
      ArtHeader* header = (ArtHeader*) buffer;
      // check packet ID, if "Art-Net", we are receiving artnet!!
      if (memcmp(header->ID, "Art-Net", 8) == 0) {
        // read the rest of the packet
        udp.read(buffer + sizeof(ArtHeader), udp.available());
        // check the op-code, and act accordingly
        switch (header->OpCode) {
          // for a poll message, send back the node capabilities
          case OpPoll: {
            if (DEBUG) Serial.println("POLL");
            node.createPollReply();
            udp.beginPacket(node.broadcastIP(), config.udpPort);
            udp.write(buffer, sizeof(ArtPollReply));
            udp.endPacket();
            break;
          }

          // for a DMX message, decode and set the leds to their new values
          case OpDmx: {
            if (DEBUG) Serial.println("DMX");
            ArtDmx* dmx = (ArtDmx*) buffer;
            int port = node.getAddress(dmx->SubUni, dmx->Net) - node.getStartAddress();
            if (port >= 0 && port < config.numPorts) {
              int max;
              if (port % universesPerOutput == 2) max = ledsOnLastUniverse;
              else max = ledsOnFullUniverse;
              int output = (int) port / universesPerOutput;
              int offset = (int) (port % universesPerOutput) * ledsOnFullUniverse;
              if (DEBUG) debugFrame(port, output, offset);
              byte* data = (byte*) dmx->Data;
              for (int i = 0; i < max; i++) {
                color_t current = {
                  (int) data[i * NUM_CHANNEL_PER_LED],
                  (int) data[i * NUM_CHANNEL_PER_LED + 1],
                  (int) data[i * NUM_CHANNEL_PER_LED + 2],
                  (int) data[i * NUM_CHANNEL_PER_LED + 3]
                };
                if (DEBUG) debugLed(data, i);
                td.setLed(output, offset + i, current);
              }
            }

            if (!SYNC) {
              // TODO: show leds!
            }
            break;
          }

          // for a sync message, show the updated state of the pixels
          case OpSync: {
            if (DEBUG) Serial.println("SYNC");
            if (SYNC) {
              // TODO: show leds!
            }
            break;
          }

          // default, do nothing since the packet hasn't been recognized as a artnet packet
          default:
            break;
        }
      }
      // check packet ID, if "Art-Ext", we are receiving artnet extended!!
      // and if OpCode is OpPoll then we can send an extended poll reply
      // that will let us inform about the 16 available universes
      else if (
        memcmp(header->ID, "Art-Ext", 8) == 0 &&
        header->OpCode == (OpPoll | 0x0001)
      ) {
        if (DEBUG) Serial.println("POLL EXTENDED");
        // read the rest of the packet
        udp.read(buffer + sizeof(ArtHeader), udp.available());
        node.createExtendedPollReply();
        udp.beginPacket(node.broadcastIP(), config.udpPort);
        udp.write(buffer, sizeof(ArtPollReply));
        udp.endPacket();
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// teensy 4.1 mac generation
static void teensyMAC(uint8_t *mac) {
  uint32_t m1 = HW_OCOTP_MAC1;
  uint32_t m2 = HW_OCOTP_MAC0;
  mac[0] = m1 >> 8;
  mac[1] = m1 >> 0;
  mac[2] = m2 >> 24;
  mac[3] = m2 >> 16;
  mac[4] = m2 >> 8;
  mac[5] = m2 >> 0;
}

///////////////////////////////////////////////////////////////////////////////
// blink test
void blink() {
  for (int i = 0; i < NUM_OF_OUTPUTS; i++) {
    td.setLed(i, 0, white);
  }
  delay(3000);
  for (int i = 0; i < NUM_OF_OUTPUTS; i++) {
    td.setLed(i, 0, black);
  }
}

///////////////////////////////////////////////////////////////////////////////
// some debug diagnostic
void diagnostic() {
  delay(5000);
  Serial.println(F_CPU);
  // Serial.println(F_BUS);
  Serial.println("MAC address");
  // prints the MAC address
  for (int i = 0; i < 6; ++i) {
    Serial.print(config.mac[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.println("");
  // prints the IP
  Serial.println("IP address");
  for (int i = 0; i < 4; ++i) {
    Serial.print(config.ip[i]);
    if (i < 3) {
      Serial.print(".");
    }
  }
  Serial.println("");
  // prints the number of ports
  Serial.print("Number of ports: ");
  Serial.println(config.numPorts);
}

///////////////////////////////////////////////////////////////////////////////
// some debug for dmx frame
void debugFrame(int port, int output, int offset) {
  Serial.print("Universe: ");
  Serial.println(port);
  Serial.print("Output: ");
  Serial.println(output);
  Serial.print("Offset: ");
  Serial.println(offset);
}

void debugLed(byte* data, int i) {
  Serial.print(data[i * NUM_CHANNEL_PER_LED]);
  Serial.print(" / ");
  Serial.print(data[i * NUM_CHANNEL_PER_LED + 1]);
  Serial.print(" / ");
  Serial.print(data[i * NUM_CHANNEL_PER_LED + 2]);
  Serial.print(" / ");
  Serial.println(data[i * NUM_CHANNEL_PER_LED + 3]);
}
