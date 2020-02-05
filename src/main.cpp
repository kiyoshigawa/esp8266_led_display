/*
This is code to run a 4x (8x8) MAS2179 display clock connected to an ESP8266.
It will connect to a wireless network, and then set the time by contacting an NTP server
It will then continuously try to connect to the NTP server to keep the time correct,
while running on its own clock crystal between connections to the NTP server.

There is also code that allows for settings to be cahnged by pressing a config button, followed by up/down adjustment buttons.
ALl settings will be saved in persistant FLASH memory using the EEPROM.h library for ESP8266
The following items can be set via this method:
  Brightness: 0-15 value, controls the brightness of all LEDs on the display.
  GMT Offset: from -12 to +14 hours per GMT specs. This will snap to any valid GMT offset per the GMT_OFFSETS enum
  12/24H Modes: This will allow switching between 24H time or 12H AM/PM time displays.
  Display Seconds: This will allow the end user to decide if they want to display seconds or now on the display.
*/

#include <Arduino.h>
#include <stdint.h>
#include <NTPClient.h>
#include <esp8266_pins.h>
#include <esp8266WIFI.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <max7219.h>
#include <fonts.h>

//wifi credentials - adjust to taste
#define WIFI_NAME "801Labs-Guest"
#define WIFI_PASS "DC801DC801"

//these are named variables to make the intent of the code more readable.
#define _12H_MODE true
#define _24H_MODE false
#define EEPROM_HAS_BEEN_INITIALIZED 1

//default brightness - can be from 0x0 to 0xF
#define DEFAULT_BRIGHTNESS 0x6U

//default display mode for time - seconds on or off
#define DEFAULT_DISPLAY_MODE false

//this controls whether or not the clock displays time in 12H or 24H mode
#define DEFAULT_12H_24H_MODE _12H_MODE

//Change this to adjust the default time zone on power up in seconds - Adjust as needed. (60 s/min * 60min/hour * (+/-)Offset in Hours)
#define DEFAULT_TIME_OFFSET (60L * 60L * -7L)

//this is how often the NTP client object will check for updates in milliseconds. (1000ms/s * 60s/min * 5 min)
#define DEFAULT_NTP_SERVER_CHECK_INTERVAL (1000UL * 60UL * 5UL)

//this timeout is for initial connections to the wifi. It will only try for this many ms.
#define WIFI_TIMEOUT 10000UL

//this is how often the wifi will try to reconnect if it becomes disconnected in ms. (1000ms/s * 60s/min * 5 min)
#define WIFI_RECONNECT_CHECK_INTERVAL (1000UL * 60UL * 5UL)

//this is how long to wait for the NTP server to send back a valid time before giving up in ms (1000ms/s * 2s)
#define NTP_CONNECTION_TIMEOUT (1000UL * 2UL)

//this is how many characters a string can be at most. Trying to display strings longer than this will result in truncation:
#define MAX_STRING_BUFFER_LENGTH 128

//this is the offset between EEPROM data values in bytes:
//set to 4U so you get 4*8 = 32 bit values for each address location
#define EEPROM_BYTE_OFFSET 4U

//this is how many bytes of EEPROM are reserved for storing settings data 
//the total is: (Number of bytes per item stored * (number of items being stored + 1U for the init address)):
//the init address will be 0 of the EEPROM has not been initialized, and any other value in the LSB if it has been.
#define NUM_EEPROM_BYTES (EEPROM_BYTE_OFFSET * (4U + 1U))

//these are the EEPROM addressed where things are stored:
//the EEPROM data is bropken into 32-bit values, Least Significant Bytes first.
#define EEPROM_INIT_ADDRESS         (0x00*EEPROM_BYTE_OFFSET)
#define EEPROM_BRIGHTNESS_ADDRESS   (0x01*EEPROM_BYTE_OFFSET)
#define EEPROM_DISPLAY_MODE_ADDRESS (0x02*EEPROM_BYTE_OFFSET)
#define EEPROM_12H_24H_ADDRESS      (0x03*EEPROM_BYTE_OFFSET)
#define EEPROM_TIME_OFFSET_ADDRESS  (0x04*EEPROM_BYTE_OFFSET)

//set this to true to force an EEPROM reset:
bool FORCE_EEPROM_INIT = false;

//this is the NTP client's UDP object.
WiFiUDP ntpUDP;

//this si the NTP client object:
NTPClient timeClient(ntpUDP, "pool.ntp.org", DEFAULT_TIME_OFFSET, DEFAULT_NTP_SERVER_CHECK_INTERVAL);

//this tracks the last wifi connection time for reconnect attempts:
uint32_t last_wifi_connection_attempt = 0;
      
//this is a variable that will track if a valid NTP time has been received since power on
//Until this is true, the clock will not display any time, only the INIT_MESSAGE
bool valid_NTP_time_received = false;

//this will keep track of the last value for seconds returned from the loop, so the display will only be updated once per second
uint8_t last_seconds = 0;

//this tracks the display mode. True means 24H time display, false is 12h time display
uint32_t display_time_in_24_h = DEFAULT_12H_24H_MODE;

//this tracks whether or not to display seconds on the clock display mode.
uint32_t display_mode = DEFAULT_DISPLAY_MODE;

//this stores the current brightness setting.
uint32_t display_brightness = DEFAULT_BRIGHTNESS;

//this stores the current UTC offset in seconds:
uint32_t current_time_offset = DEFAULT_TIME_OFFSET;

//this is a string buffer that stores the current time for use in printing time strings to the display:
uint8_t print_string_buffer[MAX_STRING_BUFFER_LENGTH];

//this is how many characters are currently being used in the string buffer:
uint8_t current_num_chars_in_buffer = 0;

//this will write all 4 bytes of EEPROM memory with a 32-bit integer value
void write_32_bit_EEPROM_value(unsigned int address, uint32_t value)
{
  for(int i=0; i<3; i++){
    //get the current LSB
    uint8_t lsb = (uint8_t)value & 0x000000FF;
    Serial.println(lsb);
    //write the current LSB to the appropriate address
    EEPROM.write(address+i, lsb);
    //shift the value over 8 bits to make the next iteration use the byte above the previous lsb
    value = value >> 8;
    EEPROM.commit();
  }
}

//this reads a 32-bit value from the EEPROM, starting at address as the Least signifigant byte, and moving throught he next three bytes after that.
uint32_t read_32_bit_EEPROM_value(unsigned int address)
{
  uint32_t combined_value = 0;
  for(int i=0; i<3; i++){
    //shift the value over by the byte number so it lands in the right place
    combined_value = combined_value & (EEPROM.read(address+i) << i*8);
  }
  return combined_value;
}

//this will initialize the EEPROM data to the default values, which can then be updated by the code or changed later.
void init_EEPROM(void)
{
  //write the init bit so that the MCU will know the EEPROM has valid initial values:
  write_32_bit_EEPROM_value(EEPROM_INIT_ADDRESS, 0x01U);
  //write the default values for the various settings options to their EEPROM addresses:
  write_32_bit_EEPROM_value(EEPROM_BRIGHTNESS_ADDRESS, DEFAULT_BRIGHTNESS);
  write_32_bit_EEPROM_value(EEPROM_DISPLAY_MODE_ADDRESS, DEFAULT_DISPLAY_MODE);
  write_32_bit_EEPROM_value(EEPROM_12H_24H_ADDRESS, DEFAULT_12H_24H_MODE);
  write_32_bit_EEPROM_value(EEPROM_TIME_OFFSET_ADDRESS, DEFAULT_TIME_OFFSET);
  //print a debug serial message to let everyone know you reset the EEPROM:
  Serial.println("EEPROM Reset to default values.");
}

void restore_from_EEPROM(void)
{
  //this will set the current values to the values currently stored in EEPROM.
  Serial.println("Reading values from EEPROM:");
  display_brightness = read_32_bit_EEPROM_value(EEPROM_BRIGHTNESS_ADDRESS);
  display_mode = read_32_bit_EEPROM_value(EEPROM_DISPLAY_MODE_ADDRESS);
  display_time_in_24_h = read_32_bit_EEPROM_value(EEPROM_12H_24H_ADDRESS);
  current_time_offset = read_32_bit_EEPROM_value(EEPROM_TIME_OFFSET_ADDRESS);
  Serial.print("Brightness set to: ");
  Serial.println(display_brightness);
  Serial.print("DIsplay Mode set to: ");
  Serial.println(display_mode);
  Serial.print("24 Hour mode set to: ");
  Serial.println(display_time_in_24_h);
  Serial.print("Current time offset from GMT in seconds set to: ");
  Serial.println(current_time_offset);
}

bool connect_to_wifi(void)
{
  uint32_t wifi_connection_timeout = millis();
  last_wifi_connection_attempt = millis();

  WiFi.begin(WIFI_NAME, WIFI_PASS);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
    if(millis() >= wifi_connection_timeout + WIFI_TIMEOUT){
      Serial.println();
      Serial.println("Unable to connect.");
      return false;
    }
  }
  Serial.println();

  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
  return true;
}

//this will force an update of the ntp time
bool update_NTP_time()
{
  uint32_t ntp_connection_start_time = millis();
  while(!timeClient.update()) {
    timeClient.forceUpdate();
    if(millis() >= NTP_CONNECTION_TIMEOUT + ntp_connection_start_time){
      Serial.println("Unable to connect to NTP server, will try again later.");
      return false;
    }
  }
  Serial.print("NTP time updated to ");
  Serial.println(timeClient.getFormattedDate());
  valid_NTP_time_received = true;
  return true;
}

//this will regularly check to make sure we're connected to the internet, and if we are, it will update the NTP time.
void verify_time()
{
  if(millis() >= last_wifi_connection_attempt + WIFI_RECONNECT_CHECK_INTERVAL){
    last_wifi_connection_attempt = millis();
    if(WiFi.status() != WL_CONNECTED){
      if(!connect_to_wifi()){
        Serial.println("Unable to connect to WIFI, will try again later.");
      }
      else{
        //force an update of the NTP time
        update_NTP_time();
      }
    }
    else{
      //force an update of the NTP time
      update_NTP_time();
      //set the clock to show the current time.
    }
  }
}

void display_error_pattern()
{
  //this is how many columsn are in the screen buffer
  uint16_t num_cols = NUM_MAX*8+8;
  for(int i=0; i<num_cols; i++){
    if(i%2){
      scr[i] = B01010101;
    }
    else{
      scr[i] = B10101010;
    }
    refreshAll();
  }
}

void print_time_from_NTP()
{
  //first get the hours, minutes, and seconds
  uint8_t hours = timeClient.getHours();
  if(!display_time_in_24_h){
    //correct to 12h time display values from 24h time values provided by ntp
    if(hours > 12){
      hours = hours - 12;
    }
    else if(hours == 0){
      hours = 12;
    }
  }
  uint8_t minutes = timeClient.getMinutes();
  uint8_t seconds = timeClient.getSeconds();
  if(last_seconds != seconds){
    last_seconds = seconds;
    //create a time string to be displayed:

  }
  else{
    last_seconds = seconds;
    //do nothing, only update when the seconds changes
  }
}

//this will output the current time to the LCD display
void display_time(void)
{
  //first make sure a valid time has been received:
  if(valid_NTP_time_received){
    //output the current time to the display:
    print_time_from_NTP();
  }
  else{
    //output an error pattern:
    display_error_pattern();
  }
}

void setup()
{
  //init for Debug messages
  Serial.begin(115200);
  Serial.println();

  //set up the EEPROM section of the flash:
  EEPROM.begin(NUM_EEPROM_BYTES);

  //check the EEPROM memory to see if it has been initialized.
  Serial.println(EEPROM.read(EEPROM_INIT_ADDRESS));
  if(EEPROM.read(EEPROM_INIT_ADDRESS) != EEPROM_HAS_BEEN_INITIALIZED || FORCE_EEPROM_INIT){
    init_EEPROM();
    Serial.println(EEPROM.read(EEPROM_INIT_ADDRESS));
  } else {
    restore_from_EEPROM();
  }

  //init displays:
  initMAX7219();
  sendCmdAll(CMD_SHUTDOWN, 1); //turn shutdown mode off
  sendCmdAll(CMD_INTENSITY, DEFAULT_BRIGHTNESS); //set brightness

  //print an init message to the display:
  display_error_pattern();

  //start the NTP Client object
  timeClient.begin();

  if(!connect_to_wifi()){
    Serial.println("Unable to connect to WIFI, will try again later.");
  }
  else{
    //force an update of the NTP time
    update_NTP_time();
  }
}

void loop()
{
  //update the ntpClient object
  timeClient.update();
  //check connectivity and update time from remote NTP servers
  verify_time();
  //display the current time if a valid time has been received.
  display_time();
}