/*
 * WiFi.cpp
 * Copyright (C) 2019-2020 Linar Yusupov
 *
 * Bug fixes and improvements
 * Copyright (C) 2020 Manuel Roesel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SoC.h"

#if defined(EXCLUDE_WIFI)
void WiFi_setup()
{}
void WiFi_loop()
{}
void WiFi_fini()
{}
#else

#include <FS.h>
#include <TimeLib.h>

#include "OTA.h"
#include "GNSS.h"
#include "EEPROM.h"
#include "WiFi.h"
#include "Traffic.h"
#include "RF.h"
#include "Web.h"
#include "NMEA.h"
#include "Battery.h"

String station_ssid = "ognbase";
String station_psk  = "123456789";

String host_name = HOSTNAME;

IPAddress local_IP(192, 168, 1, 1);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

/**
 * Default WiFi connection information.
 *
 */
const char* ap_default_psk = "987654321"; ///< Default PSK.

#if defined(USE_DNS_SERVER)
#include <DNSServer.h>

const byte DNS_PORT = 53;
DNSServer  dnsServer;
bool       dns_active = false;
#endif

// A UDP instance to let us send and receive packets over UDP
WiFiUDP Uni_Udp;

unsigned int RFlocalPort = RELAY_SRC_PORT;      // local port to listen for UDP packets

char UDPpacketBuffer[256]; // buffer to hold incoming and outgoing packets

#if defined(POWER_SAVING_WIFI_TIMEOUT)
static unsigned long WiFi_No_Clients_Time_ms = 0;
#endif

/**
 * @brief Read WiFi connection information from file system.
 * @param ssid String pointer for storing SSID.
 * @param pass String pointer for storing PSK.
 * @return True or False.
 *
 * The config file have to containt the WiFi SSID in the first line
 * and the WiFi PSK in the second line.
 * Line seperator can be \r\n (CR LF) \r or \n.
 */
bool loadConfig(String* ssid, String* pass)
{
    int line = 0;

    // open file for reading.
    File configFile = SPIFFS.open("/ogn_conf.txt", "r");
    if (!configFile)
    {
        Serial.println(F("Failed to open ogn_conf.txt."));
        return false;
    }

    while (configFile.available() && line < 5)
    {
        if (line == 0)
            *ssid = configFile.readStringUntil('\n').c_str();
        if (line == 1)
            *pass = configFile.readStringUntil('\n').c_str();
        line++;
    }

    configFile.close();

    if (line < 2)
        return false;

    ssid->trim();
    pass->trim();


    return true;
} // loadConfig

/**
 * @brief Save WiFi SSID and PSK to configuration file.
 * @param ssid SSID as string pointer.
 * @param pass PSK as string pointer,
 * @return True or False.
 */
/*bool saveConfig(String *ssid, String *pass)
   {
   // Open config file for writing.
   File configFile = SPIFFS.open("/ogn_conf.txt", "w");
   if (!configFile)
   {
    Serial.println(F("Failed to open ogn_conf.txt for writing"));
    return false;
   }

   // Save SSID and PSK.
   configFile.println(*ssid);
   configFile.println(*pass);
   configFile.close();

   return true;
   } // saveConfig*/

size_t Raw_Receive_UDP(uint8_t* buf)
{
    int noBytes = Uni_Udp.parsePacket();
    if (noBytes)
    {
        if (noBytes > MAX_PKT_SIZE)
            noBytes = MAX_PKT_SIZE;

        // We've received a packet, read the data from it
        Uni_Udp.read(buf, noBytes); // read the packet into the buffer

        return (size_t) noBytes;
    }
    else
        return 0;
}

void Raw_Transmit_UDP()
{
    size_t rx_size = RF_Payload_Size(settings->rf_protocol);
    rx_size = rx_size > sizeof(fo.raw) ? sizeof(fo.raw) : rx_size;
    String str = Bin2Hex(fo.raw, rx_size);
    size_t len = str.length();
    // ASSERT(sizeof(UDPpacketBuffer) > 2 * PKT_SIZE + 1)
    str.toCharArray(UDPpacketBuffer, sizeof(UDPpacketBuffer));
    UDPpacketBuffer[len] = '\n';
    SoC->WiFi_transmit_UDP(RELAY_DST_PORT, (byte *)UDPpacketBuffer, len + 1);
}

/**
 * @brief Arduino setup function.
 */
void WiFi_setup()
{
#if 1
    // Initialize file system.
    if (!SPIFFS.begin(true))
    {
        Serial.println(F("Failed to mount file system"));
        return;
    }

    // Load wifi connection information.
    if (!loadConfig(&station_ssid, &station_psk))
    {
        station_ssid = MY_ACCESSPOINT_SSID;
        station_psk  = MY_ACCESSPOINT_PSK;

        Serial.println(F("No WiFi connection information available."));
    }
#endif

    // Check WiFi connection
    // ... check mode
    if (WiFi.getMode() != WIFI_STA)
    {
        WiFi.mode(WIFI_STA);
        delay(10);
    }

    // ... Compare file config with sdk config.
    if (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk)
    {
        Serial.println(F("WiFi config changed."));

        // ... Try to connect to WiFi station.
        WiFi.begin(station_ssid.c_str(), station_psk.c_str());

        // ... Pritn new SSID
        Serial.print(F("new SSID: "));
        Serial.println(WiFi.SSID());

        // ... Uncomment this for debugging output.
        //WiFi.printDiag(Serial);
    }
    else
        // ... Begin with sdk config.
        WiFi.begin();

    // Set Hostname.
    host_name += String((SoC->getChipId() & 0xFFFFFF), HEX);
    SoC->WiFi_hostname(host_name);

    // Print hostname.
    Serial.println("Hostname: " + host_name);

    Serial.println(F("Wait for WiFi connection."));

    // ... Give ESP 10 seconds to connect to station.
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000)
    {
        Serial.write('.');
        //Serial.print(WiFi.status());
        delay(500);
    }
    Serial.println();

    // Check connection
    if (WiFi.status() == WL_CONNECTED)
    {
        // ... print IP Address
        Serial.print(F("IP address: "));
        Serial.println(WiFi.localIP());
    }
    else
    {
        Serial.println(F("Can not connect to WiFi station. Go into AP mode.")); // if (WiFi.status() != WL_CONNECTED) Connect();

        // Go into software AP mode.
        WiFi.mode(WIFI_AP);
        SoC->WiFi_setOutputPower(WIFI_TX_POWER_MED); // 10 dB
        // WiFi.setOutputPower(0); // 0 dB
        //system_phy_set_max_tpw(4 * 0); // 0 dB
        delay(10);

        Serial.print(F("Setting soft-AP configuration ... "));
        Serial.println(WiFi.softAPConfig(local_IP, gateway, subnet) ?
                       F("Ready") : F("Failed!"));

        Serial.print(F("Setting soft-AP ... "));
        Serial.println(WiFi.softAP(host_name.c_str(), ap_default_psk) ?
                       F("Ready") : F("Failed!"));
#if defined(USE_DNS_SERVER)
        // if DNSServer is started with "*" for domain name, it will reply with
        // provided IP to all DNS request
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
        dns_active = true;
#endif
        Serial.print(F("IP address: "));
        Serial.println(WiFi.softAPIP());
    }

    Uni_Udp.begin(RFlocalPort);
    Serial.print(F("UDP server has started at port: "));
    Serial.println(RFlocalPort);

#if defined(POWER_SAVING_WIFI_TIMEOUT)
    WiFi_No_Clients_Time_ms = millis();
#endif
}

bool Wifi_connected()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;
    else
        return true;
}

void WiFi_loop()
{
#if defined(USE_DNS_SERVER)
    if (dns_active)
        dnsServer.processNextRequest();

#endif

#if defined(POWER_SAVING_WIFI_TIMEOUT)
    if ((settings->power_save & POWER_SAVE_WIFI) && WiFi.getMode() == WIFI_AP)
    {
        if (SoC->WiFi_clients_count() == 0)
        {
            if ((millis() - WiFi_No_Clients_Time_ms) > POWER_SAVING_WIFI_TIMEOUT)
            {
                NMEA_fini();
                Web_fini();
                WiFi_fini();

                if (settings->nmea_p)
                    StdOut.println(F("$PSRFS,WIFI_OFF"));
            }
        }
        else
            WiFi_No_Clients_Time_ms = millis();
    }
#endif
}

void WiFi_fini()
{
    Uni_Udp.stop();

    WiFi.mode(WIFI_OFF);
}

#endif /* EXCLUDE_WIFI */
