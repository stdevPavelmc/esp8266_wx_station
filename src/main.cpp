// Pavel Milanes
// DIY weather station reporting directly via influx

 /**********************************************
 *  Users configs here
 ************************************************/

#define DEBUG 1
//#define INFLUXDEBUG 1

// Arduino
#include <Arduino.h>
#define VERSION					"v0.1.6"		// version
#define WIFI_SSID       "Opi"				// Wifi user
#define WIFI_PASS       "Xilantro"	// Wifi passwd
#define INFLUXDB_HOST   "10.42.1.1"	// influx server
#define INFLUXDB_DBNAME "test"			// name of the influxdb database to insert data 

// Wifi related functions
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

// WIFI static IP settings
IPAddress staticIP(10, 42, 1, 2);		// ESP8266 static ip
IPAddress gateway(10, 42, 1, 1);		//IP Address of your WiFi Router (Gateway)
IPAddress subnet(255, 255, 255, 0); //Subnet mask
IPAddress dns(10, 42, 1, 1);				//DNS
const char* host = "meteo";					

// OTA related configs
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// web server
ESP8266WebServer server(80);

// Web auth
const char* www_username = "admin";
const char* www_password = "esp8266";
const char* www_realm = "Custom Auth Realm";
String authFailResponse = "Authentication Failed";

// some HTML vars
String htmlHeadStatic = "";
String htmlHeadRefresh = "";
String htmlTail = "";
String HTML = "";

// I2C include
#include <Wire.h>

// influxdb
#include <InfluxDb.h>
Influxdb influx(INFLUXDB_HOST);

// BMP180
#include <SFE_BMP180.h>
SFE_BMP180 bmp;

// DHT11
#include <DHTesp.h>
DHTesp dht;

// BH1705 lux metter
#include <BH1750FVI.h>
BH1750FVI BH1750(BH1750_DEFAULT_I2CADDR, BH1750_CONTINUOUS_HIGH_RES_MODE_2, BH1750_SENSITIVITY_DEFAULT, BH1750_ACCURACY_DEFAULT);

// vars
float bmpPress = 0;
float bmpTemp = 0;
float dhtHum = 0;
float dhtTemp = 0;
float dhtTempFeel = 0;
float dhtDewPoint = 0;
int   dhtComfort = 0;
float bhLight = 0;

// Analog 5 voltage as measured on a VCC pin on the arduno pro-mini
#define V5 4.934
#define VOLTSCALE 3.102
#define CURRSCALE 20.0
#define RAINMM 12.5

// raw values from the second Arduino
int adc_max_sampling;
int battery = 0;
int lm35 = 0;
int current = 0;
int windir = 0;

// this ones are used as raw values
int rainCount = 0;

// masurements from the arduno pro-mini
float batteryV = 0;
float lm35Temp = 0;
float windDir = 0;
float batteryCurrent;
float winddir = 0;

// other
int Lightnings = 0;
int WindSpeed = 0;
float RainAmount = 0;

/* enum PerceptionState {
Perception_Dry = 0,
Perception_VeryComfy = 1,
Perception_Comfy = 2,
Perception_Ok = 3,
Perception_UnComfy = 4,
Perception_QuiteUnComfy = 5,
Perception_VeryUnComfy = 6,
Perception_SevereUncomfy = 7
*/

// internal vars
unsigned long lastmillis = millis();
# define INTERVAL 30

/*****************************************************************/

void bmp180Read() {
	int status;

	// You need to measure temp first, it will return 0 on fail or
	// ms to wait to get the temp
	status = bmp.startTemperature();
	if (status != 0) {
		// Wait for the measurement to complete:
		delay(status);

		// declare vars
		double T;

		// Retrieve the completed temperature measurement:
		// Note that the measurement is stored in the variable T.
		// Function returns 1 if successful, 0 if failure.
		status = bmp.getTemperature(T);
		if (status != 0) {
			
			// Start a bmp180 measurement:
			// The parameter is the oversampling setting, from 0 to 3 (highest res, longest wait).
			// If request is successful, the number of ms to wait is returned.
			// If request is unsuccessful, 0 is returned.
			status = bmp.startPressure(3);
			if (status != 0) {
				// Wait for the measurement to complete:
				delay(status);

				// declare vars
				double P;

				// Retrieve the completed bmp180 measurement:
				// Note that the measurement is stored in the variable P.
				// Note also that the function requires the previous temperature measurement (T).
				// (If temperature is stable, you can do one temperature measurement for a number of bmp180 measurements.)
				// Function returns 1 if successful, 0 if failure.
				status = bmp.getPressure(P,T);
				if (status != 0) {

					// set the external vars
					bmpPress = P / 10;
					bmpTemp = T - 4.10;	// correction based on my alcohol thermometer
				}
			}
		}
	}
}

void dhtRead() {
	// DHT measurements if status is ok
	if (dht.getStatusString() == "OK")  {
		dhtHum = dht.getHumidity();
		dhtTemp = dht.getTemperature();
		dhtTempFeel = dht.computeHeatIndex(dhtTemp, dhtHum, false);
		dhtDewPoint = dht.computeDewPoint(dhtTemp, dhtHum, false);
		dhtComfort = dht.computePerception(dhtTemp, dhtHum, false);
	} else {
		Serial.println("DHT sensor is not ready, reads time to short?");
	}
}

void bhRead() {
	// read the values for the Lux and
	bhLight = BH1750.readLightLevel();
}

float percent(int val) {
	return (float(val) / adc_max_sampling);
}

float calcCurrent(int val) {
	// calc the voltage diff against the center
	float vdiff = percent((val - (adc_max_sampling / 2)) * V5);

	// calc the current associated
	float curr = (vdiff / (adc_max_sampling / 2)) * CURRSCALE;

	// return
	return curr;
}

void updateValuesFromArduino() {
	// update ADC values (volt scale is applied before percent to increase accuracy)
	batteryV = percent(battery * V5) * VOLTSCALE;
	lm35Temp = (percent(lm35 * V5 ) / 0.01) + 1.30; // << direct temp conversion + correction
	windDir = 0; // TODO use BMUX to set the values
	batteryCurrent = calcCurrent(current);

	// non ADc values
	RainAmount = rainCount * RAINMM;
}

int getValue(){
  // read two bytes from the I2C MSBF and return it as a word
  int reading;
  reading = int(Wire.read()) << 8;
  reading |= Wire.read();
  return reading;
}

void getI2CData() {
	// size of the data to get
	byte count = 16;

	// empty the buffer for the next reading
	while(Wire.available()) {
		byte discard = Wire.read();
	}

	// request the data
	byte available = Wire.requestFrom(0x21, count);

	// check
	if (available == count) {
		// will be used to calc
		adc_max_sampling = getValue();
		battery = getValue();
		lm35 = getValue();
		current = getValue();
		windir = getValue();
		
		// will be used
		Lightnings = getValue();
		WindSpeed = getValue();
		rainCount = getValue();
	}

	// update values
	updateValuesFromArduino();
}

void takeSamples() {
	// read temp an press
	bmp180Read();

	// read temp and humidity
	dhtRead();

	// BH1705 lux data
	bhRead();

	// get data via I2C from the other arduino
	getI2CData();
}

void measure() {
	// wrapper of the get remote & process local
	getI2CData();
	takeSamples();
}

InfluxData measure(char* rowName, float val, char *device, char *sensor, char *place, char *comm) {
	InfluxData row(rowName);
	row.addTag("device", device);
	row.addTag("sensor", sensor);
	row.addTag("place", place);
	row.addTag("comment", comm);
	row.addValue("value", val);
	return row;
}

void infxSendData() {
	// create measurements
	InfluxData m0 = measure("temperature", bmpTemp, "WX_stat", "BMP180", "Techo", "real");
	influx.prepare(m0);

	InfluxData m1 = measure("pression", bmpPress, "WX_stat", "BMP180", "Techo", "real");
	influx.prepare(m1);

	InfluxData m2 = measure("temperature", dhtTemp, "WX_stat", "DHT11", "Techo", "real");
	influx.prepare(m2);

	InfluxData m3 = measure("temperature", dhtTempFeel, "WX_stat", "DHT11", "Techo", "feel");
	influx.prepare(m3);

	InfluxData m4 = measure("humidity", dhtHum, "WX_stat", "DHT11", "Techo", "real");
	influx.prepare(m4);

	InfluxData m5 = measure("battery", batteryV, "WX_stat", "Battery", "Techo", "real");
	influx.prepare(m5);

	InfluxData m6 = measure("light", bhLight, "WX_stat", "BH1705", "Techo", "real");
	influx.prepare(m6);

	InfluxData m7 = measure("temperature", lm35Temp, "WX_stat", "LM35", "Techo", "feel");
	influx.prepare(m7);

	InfluxData m8 = measure("lightnings", Lightnings, "WX_stat", "lightnings", "Techo", "real");
	influx.prepare(m8);

	InfluxData m9 = measure("windSpeed", WindSpeed, "WX_stat", "WindSpeed", "Techo", "real");
	influx.prepare(m9);

	InfluxData mA = measure("windDir", windDir, "WX_stat", "WindDirection", "Techo", "real");
	influx.prepare(mA);

	InfluxData mB = measure("rainAmount", RainAmount, "WX_stat", "RainAmount", "Techo", "real");
	influx.prepare(mB);

	// actual write
	influx.write();
}

void serialDebug() {
	static byte pcount = 11;
	
	// header every 10 reads
	if (pcount >= 10){
		Serial.print("Temp (bmp/dht/LM35)(C)\tHum(%)\tPress(kPa)\tLight(%)\tBattery (v)");
		Serial.println("\tWSpeed(%)\tRain(mm/s)\tLightning");
		pcount = 1;
	} else {
		pcount++;
	}
	
	// serial debug info
	Serial.print(bmpTemp, 1);
	Serial.print(" / ");
	Serial.print(dhtTemp, 1);
	Serial.print(" / ");
	Serial.print(lm35Temp, 1);
	Serial.print("\t");
	Serial.print(dhtHum, 1);
	Serial.print("\t");
	Serial.print(bmpPress, 1);
	Serial.print("\t\t");
	Serial.print(bhLight, 1);
	Serial.print("\t\t");
	Serial.print(batteryV, 3);
	Serial.print("\t\t");
	Serial.print(WindSpeed, 1);
	Serial.print("\t\t   ");
	Serial.print(RainAmount, 1);
	Serial.print("\t\t");
	Serial.println(Lightnings);
}

void handleIndex() {
	// craft the index page
	String HTML = htmlHeadRefresh;
	HTML += "<h1>Condiciones meteo Ahora</h1><hr><br/>";
	HTML += "<table><tr><td>Temp:</td><td>";
	HTML += lm35Temp;
	HTML += " Celcius</td></tr>";
	HTML += "<tr><td>Hum:</td><td>";
	HTML += dhtHum;
	HTML += " H</td></tr>";
	HTML += "<tr><td>Press:</td><td>";
	HTML += bmpPress;
	HTML += " hPa</td></tr>";
	HTML += "</table><br/>";
	HTML += "<a href='up'>Upgradear el firmware</a>";
	HTML += htmlTail;

	// add it
	server.sendHeader("Connection", "close");
	server.send(200, "text/html", HTML);
}

void handleUpdate() {
	// auth for this one
	if (!server.authenticate(www_username, www_password)) {
    	return server.requestAuthentication(DIGEST_AUTH, www_realm, authFailResponse);
    }

	// craft the index page
	String HTML = htmlHeadStatic;
	HTML += "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form>";
	HTML += htmlTail;

	// add it
	server.sendHeader("Connection", "close");
	server.send(200, "text/html", HTML);
}

void handlePostResult() {
	server.sendHeader("Connection", "close");
	server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
	ESP.restart();
}

void handlePostProcess() {
	HTTPUpload& upload = server.upload();
	if (upload.status == UPLOAD_FILE_START) {
		Serial.setDebugOutput(true);
		WiFiUDP::stopAll();
		Serial.printf("Update: %s\n", upload.filename.c_str());
		uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
		if (!Update.begin(maxSketchSpace)) { //start with max available size
			Update.printError(Serial);
		}
	} else if (upload.status == UPLOAD_FILE_WRITE) {
		if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
			Update.printError(Serial);
		}
	} else if (upload.status == UPLOAD_FILE_END) {
		if (Update.end(true)) { //true to set the size to the current progress
			Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
		} else {
			Update.printError(Serial);
		}
		Serial.setDebugOutput(false);
	}
	
	yield();
}

void handleNotFound() {
	String message = "File Not Found\n\n";
	message += "URI: ";
	message += server.uri();
	message += "\nMethod: ";
	message += (server.method() == HTTP_GET) ? "GET" : "POST";
	message += "\nArguments: ";
	message += server.args();
	message += "\n";

	for (uint8_t i = 0; i < server.args(); i++) {
		message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
	}

	server.send(404, "text/plain", message);
}

void forceConnect() {
	if (WiFi.waitForConnectResult() != WL_CONNECTED) {
		// user feedback
		Serial.println("Connecting to WIFI");

		WiFi.disconnect();
		WiFi.hostname(host); // DHCP Hostname 
		WiFi.config(staticIP, subnet, gateway, dns);
		WiFi.mode(WIFI_STA); // WIFI_STA: WiFi mode station (connect to wifi router only)
		WiFi.begin(WIFI_SSID, WIFI_PASS);

		// sleep the wifi modem
		/*** 
		 * auto light sleep: LIGHT_SLEEP_T (2mA)
		 * modem sleep: NONE_SLEEP_T (18 mA)
		*/
		wifi_set_sleep_type(LIGHT_SLEEP_T);
	}
}

void OTASetup() {
	forceConnect();

	// if not connected reboot.
	if (WiFi.status() != WL_CONNECTED) {
		// message, wait 5 seconds and reboot
		Serial.println("No connection wait 5 seconds and reboot...");
		delay(5000);
		ESP.reset();
	}

	// define some of the HTML vars
	htmlHeadRefresh = "<html><head><meta http-equiv='refresh' content='30'/><title>MeteoUpdater ";
	htmlHeadRefresh += VERSION;
	htmlHeadRefresh += " </title></head><body>";
	htmlHeadStatic = "<html><head><title>MeteoUpdater ";
	htmlHeadStatic += VERSION;
	htmlHeadStatic += " </title></head><body>";
	htmlTail = "</body></html>";

	// register the name
	MDNS.begin(host);

	server.on("/", HTTP_GET, handleIndex);
	server.on("/up", HTTP_GET, handleUpdate);
	server.on("/update", HTTP_POST, handlePostResult, handlePostProcess);
	server.onNotFound(handleNotFound);

	// start server
	server.begin();

	// advice on the http server
	MDNS.addService("http", "tcp", 80);

	// user feedback
	Serial.printf("Ready! Open http://%s.local in your browser\n", host);
}

void setup() {
	// initial pause
	delay(1000);

	// serial
	Serial.begin(115200);
	Serial.println(" ");
	Serial.print("Meteo Station ");
	Serial.println(VERSION);
	
	// Start I2C comms
	Wire.begin();

	// BMP180, checks the sensor ID and reads the calibration parameters.  
	if (!bmp.begin()) {
		while (1) {
			Serial.println("BMP180 begin failed. check your BMP180 Interface and I2C Address.");
			delay(5000);
		}
	}

	// check for the device (SDA - D2, SCL - D1)
	while (BH1750.begin(D2, D1) != true) {
		while (1) {
			Serial.println("BH1705 begin failed. check your BH1705 Interface and I2C Address.");;
			delay(5000);
		}
	}

	// DHT11
	dht.setup(D0, DHTesp::DHT11);

	// influx config here
	influx.setDb(INFLUXDB_DBNAME);
	#ifdef INFLUXDEBUG
	influx.debug = true;
	#endif

	// OTA 
	OTASetup();

	// initial measurement to populate values
	measure();
}

void loop() {
	// timing
	if ((millis() - lastmillis) / 1000 > INTERVAL) {
		// reset timer
		lastmillis = millis();

		// check for connectivity
		forceConnect();

		// measure
		measure();

		// send influx data
		infxSendData();

		// serial debug
		#ifdef DEBUG
		serialDebug();
		#endif
	}

	// OTA
	server.handleClient();
  	MDNS.update();

	// delay to trigger the sleep
	delay(1); 	
}
