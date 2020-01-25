// Pavel Milanes
// DIY weather station reporting directly via influx

 /**********************************************
 *  Users configs here
 ************************************************/

//#define DEBUG 1
//#define INFLUXDEBUG 1

// Arduino
#include <Arduino.h>
#define VERSION					"v0.5.5"	// version
#define WIFI_SSID       "Opi"		// Wifi user
#define WIFI_PASS       "Xilantro"	// Wifi passwd
#define INFLUXDB_HOST   "10.42.1.1"	// influx server
#define INFLUXDB_DBNAME "test"		// name of the influxdb database to insert data

// Wifi related functions
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

// instantiate the wificlient
WiFiClient client;

// WIFI static IP settings
IPAddress staticIP(10, 42, 1, 2);		// ESP8266 static ip
IPAddress gateway(10, 42, 1, 1);		//IP Address of your WiFi Router (Gateway)
IPAddress subnet(255, 255, 255, 0); //Subnet mask
const char* host = "meteo";

/******* Web listen trick
 * 
 * We use a trick to sleep between data send to save power
 * But we need also a trick to enable the web server and allow
 * for upgrades via wifi (OTA)
 * 
 * The trick is simple: 
 * it will check for a wakehost connection tcp 8266 port
 * if it's open it will send "OK" to it and will arm a
 * X minute timer for the web server and upload options
 * after that it will resume to sleep between data send
 * 
 * How to manage that? on linux is as simple as 
 * 
 * nc -k4l 8266
 * 
 * The k parameter is to keep it open, omit it to
 * trigger it just once like this
 * 
 * nc -4l 8266
 * 
 * Also after you has activated the timer if you keep the web
 * open it will keep rearming the timereach 30 seconds.
 * 
 * This can be also accomplished by opening the web on the 30 seconds
 * following a restart of the MCU
 ***************************************************************/
const char* wakeHost = "10.42.1.1";
int wakeHostPort = 8266;
#define WAKE_TIMER 1	// minute(s)
unsigned long wakeExpireAt = millis();
bool wifiOn = false;	// wifi state (start off) until the first read of the sensors and send
bool stayAlive = false;

// OTA related configs
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// web server
ESP8266WebServer server(80);

// Web auth
const char* www_username = "admin";
const char* www_password = "esp8266";
const char* www_realm = "Restricted area";
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
BH1750FVI BH1750(BH1750_DEFAULT_I2CADDR, BH1750_ONE_TIME_HIGH_RES_MODE, BH1750_SENSITIVITY_DEFAULT, BH1750_ACCURACY_DEFAULT);

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
#define V5 4.93
#define VOLTSCALE 3.102
#define CURRSCALE 20.0
#define RAINMM 12.5
#define LOWBATT 10.0

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

// other
int Lightnings = 0;
int WindSpeed = 0;
float RainAmount = 0;

// internal vars
int INTERVAL = 180; // seconds between intervals
unsigned long lastTime = millis();

/*****************************************************************/

void goneAsleep() {
	Serial.print("Gone asleep for ");
	Serial.print(INTERVAL);
	Serial.print(" seconds");
	
	ESP.deepSleep(INTERVAL * 1000000);
}

String strConfort(int cfindex) {
	String result = "";
	switch (cfindex)	{
	case 0:
		result = "Dry";
		break;
	case 1:
		result = "Very Comfortable";
		break;
	case 2:
		result = "Comfortable";
		break;
	case 3:
		result = "Ok";
		break;
	case 4:
		result = "Uncomfortable";
		break;
	case 5:
		result = "Quite uncomfortable";
		break;
	case 6:
		result = "Very uncomfortable";
		break;
	case 7:
		result = "Severe uncomfortable";
		break;

	default:
		result = "Unknow";
		break;
	}

	return result;
}

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
			// ok, good temp measurement
			// apply correction
			T -= 6.0;
			
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
					bmpPress = P;
					bmpTemp = T;
				}
			}
		}
	}
}

void dhtRead() {
	// DHT measurements if status is ok
	if (dht.getStatusString() == "OK")  {
		dhtHum = dht.getHumidity();
		dhtTemp = dht.getTemperature() - 1.4;
		dhtTempFeel = dht.computeHeatIndex(dhtTemp, dhtHum, false);
		dhtDewPoint = dht.computeDewPoint(dhtTemp, dhtHum, false);
		dhtComfort = dht.computePerception(dhtTemp, dhtHum, false);
	} else {
		Serial.println("DHT sensor is not ready, reads time to short?");
	}
}

void bhRead() {
	// read the values for the light sensor
	// the sensor has a screen and the calculated attenuation
	// is compensated here:
	// 
	// (2324,2+2365,8+2366,7+2354,2) ÷ 4 = 2352,725
	// (256,7+252,5+258,3+257,5) ÷ 4     = 256,25
	//
	// sunscreen attenuation is 9.181365854
	bhLight = BH1750.readLightLevel() * 9.181365854;
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
	lm35Temp = percent(lm35 * V5 * 100) - 0.2 ;
	windDir = windir;
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

	InfluxData mC = measure("temperature", dhtDewPoint, "WX_stat", "DHT11", "Techo", "DewPoint");
	influx.prepare(mC);

	InfluxData mD = measure("confortRatio", dhtComfort, "WX_stat", "ConfortRatio", "Techo", "real");
	influx.prepare(mD);

	InfluxData mE = measure("wifi_rssi", WiFi.RSSI(), "WX_stat", "WiFi_RSSI_LEVEL", "Techo", "real");
	influx.prepare(mE);

	// actual write & catch wifi errors
	influx.write();

}

#ifdef DEBUG
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
#endif

char *uptime() {
	unsigned long milli = millis();
	static char _return[32];
	unsigned long secs = milli / 1000, mins = secs / 60;
	unsigned int hours = mins / 60, days = hours / 24;
	milli -= secs * 1000;
	secs -= mins * 60;
	mins -= hours * 60;
	hours -= days * 24;
	sprintf(_return,"Uptime %d days %2.2d:%2.2d:%2.2d.%3.3d", (byte)days, (byte)hours, (byte)mins, (byte)secs, (int)milli);
	return _return;
}

void wakeArm() {
	// arm the timer for the wake
	wakeExpireAt = millis() + WAKE_TIMER * 1000 * 60;	// WAKE_TIMER minutes
	stayAlive = true;

	#ifdef DEBUG
	Serial.println("Wake trigger armed!");
	#endif // DEBUG
}

void checkWake() {
	// Use WiFiClient class to create TCP connections
  if (client.connect(wakeHost, wakeHostPort)) {
		// user feedback
    client.println(String("Wake triggered for the next ") + String(WAKE_TIMER) + String(" minutes."));
		client.flush();

		// close client
		client.stop();
		
		// ok, arm trigger
		wakeArm();
  }
} 

void handleIndex() {
	// String table helpers
	const String RowStart = "<tr><td><p align='right'><b>";
	const String RowMiddle = "</b></p></td><td><p align='left'>";
	const String RowEnd = "</p></td></tr>";
	// craft the index page
	String HTML = htmlHeadRefresh;

	HTML += "<div align='center'>";
	HTML += "<h1>Actual WX Conditions</h1><hr><br/>";
	HTML += "<p align='center'>"+ String(uptime()) + "</br>";
	HTML += "<i>Sampling each "+ String(int(INTERVAL/60)) + " minute(s)</i>";

	if (batteryV < LOWBATT) {
		HTML += "</br><div color='red'> <b>LOW BATTERY!</b></div>";
	}

	HTML += "</p>";
	HTML += "<table>";

	HTML += RowStart + "Temperature:" + RowMiddle + String(lm35Temp) + " Celcius" + RowEnd;
	HTML += RowStart + "Temperature Sensation:" + RowMiddle + String(dhtTempFeel) + " Celcius" + RowEnd;
	HTML += RowStart + "Dew Point:" + RowMiddle + String(dhtDewPoint) + " Celcius" + RowEnd;
	HTML += RowStart + "Confort Perception:" + RowMiddle + strConfort(dhtComfort) + RowEnd;
	HTML += RowStart + "Hum:" + RowMiddle + String(dhtHum) + " %H" + RowEnd;
	HTML += RowStart + "Press:" + RowMiddle + String(bmpPress) + " hPa" + RowEnd;
	HTML += RowStart + "Battery:" + RowMiddle + String(batteryV) + " V" + RowEnd;
	HTML += RowStart + "Light:" + RowMiddle + String(bhLight) + " lux" + RowEnd;
	HTML += RowStart + "WiFi AP signal level:" + RowMiddle + String(WiFi.RSSI()) + " dBm" + RowEnd;

	HTML += "</table><br/>";

	HTML += "<p><a href='up'>Firmware Upgrade</a></p>";
	
	HTML += "<hr>";
	
	HTML += "<div><i>DIY meteo station by @pavelmc, CO7WT</i></div>";
	
	HTML += "</div>";
	HTML += htmlTail;

	// add it
	server.sendHeader("Connection", "close");
	server.send(200, "text/html", HTML);

	// arm the wake timer
	wakeArm();
}

void handleUpdate() {
	// auth for this one
	if (!server.authenticate(www_username, www_password)) {
    	return server.requestAuthentication(DIGEST_AUTH, www_realm, authFailResponse);
    }

	// craft the index page
	String HTML = htmlHeadStatic;
	HTML += "<div align='center'><form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form></div>";
	HTML += htmlTail;

	// add it
	server.sendHeader("Connection", "close");
	server.send(200, "text/html", HTML);

	// arm the wake timer (to avoid the wifi of when in an update process)
	wakeArm();
}

void handlePostResult() {
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
			// prepare final msg
			HTML  = htmlHeadStatic;
			HTML += "<p>Ready! click <a href='http://";
			HTML += host;
			HTML += ".local'>here</a> to go back to start page</p>";
			
			server.sendHeader("Connection", "close");
			server.send(200, "text/html", (Update.hasError()) ? "<b>FAIL! need to upgrade via serial</b>" : HTML);

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

void WiFiOn() {
	WiFi.forceSleepWake();
	wifiOn = true;
}

void WiFiOff() {
	WiFi.forceSleepBegin();
	wifiOn = false;
	stayAlive = false;
}

bool forceConnect() {
	// enable Wifi
	WiFiOn();

	// wifi config
	WiFi.mode(WIFI_STA);
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	WiFi.hostname(host);
	WiFi.config(staticIP, gateway, subnet);
	WiFi.setSleepMode(WIFI_MODEM_SLEEP); // WIFI_LIGHT_SLEEP vs WIFI_MODEM_SLEEP
	WiFi.setAutoReconnect(true);

	if (WiFi.waitForConnectResult() != WL_CONNECTED) {
		// user feedback
		#ifdef DEBUG
		Serial.println("Not Connected to Wifi...");
		return false;
		#else
		goneAsleep();
		#endif
	} else {
		#ifdef DEBUG
		Serial.print("Connected to WIFI, IP: ");
		Serial.println(WiFi.localIP());
		#endif
		return true;
	}
}

void OTASetup() {
	// if not connected reboot/sleep
	if (WiFi.status() != WL_CONNECTED) {
		// message, wait 5 seconds and reboot
		#ifdef DEBUG
		Serial.println("No connection wait 30 seconds and reboot...");
		delay(30000);
		ESP.reset();
		#else
		goneAsleep();
		#endif
	}

	// define some of the HTML vars
	htmlHeadRefresh = "<html><head><meta http-equiv='refresh' content='20'/><title>MeteoUpdater ";
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

bool battisok() {
	// inverse logic
	if (batteryV < LOWBATT and batteryV > 1.0) {
		return false;
	} else {
		return true;
	}
}

void setup() {
	// if debug set verbose debug
	#ifdef DEBUG
	Serial.setDebugOutput(true);
	#endif // DEBUG

	// wifi off from start
	WiFiOff();

	#ifdef DEBUG
	// initial pause
	delay(2000);
	#endif // DEBUG
	
	// serial
	Serial.begin(115200);
	Serial.println(" ");
	#ifdef DEBUG
	Serial.print("Meteo Station ");
	Serial.println(VERSION);
	#endif // DEBUG

	// Start I2C comms
	Wire.begin();

	// BMP180, checks the sensor ID and reads the calibration parameters.  
	while (!bmp.begin()) {
		Serial.println("BMP180 begin failed. check your BMP180 Interface and I2C Address.");
		#ifdef DEBUG
		delay(5000);
		#else
		goneAsleep();
		#endif
	}

	// check for the device (SDA - D2, SCL - D1)
	while (BH1750.begin(D2, D1) != true) {
		Serial.println("BH1705 begin failed. check your BH1705 Interface and I2C Address.");;
		#ifdef DEBUG
		delay(5000);
		#else
		goneAsleep();
		#endif
	}

	// DHT11
	dht.setup(D3, DHTesp::DHT11);

	// influx config here
	influx.setDb(INFLUXDB_DBNAME);
	#ifdef INFLUXDEBUG
	influx.debug = true;
	#endif

	/****** ACTION GOES  HERE ***************/

	// initial measurement to populate values
	#ifdef DEBUG
	Serial.println("Reading sensors...");
	#endif
	takeSamples();

	// save power if on critical situation
	if (battisok()) {
		#ifdef DEBUG
		Serial.println("Battery is OK...");
		#endif
		
		// connect to wifi or die
		forceConnect();

		// send influx data
		infxSendData();

		#ifdef DEBUG
		Serial.println("Data sent to server...");
		#endif

		// check for wake flag
		checkWake();
	} else {
		Serial.println("Battery is LOW, not connecting to wifi...");
	}

	if (stayAlive) {
		// OTA 
		OTASetup();
	} else {
		goneAsleep();
	}
}

void loop() {
	// loop is only reached if "stayAlive"  is true
	
	// delay to trigger the light sleep in wifi
	delay(1);

	// handle wifi connections
	server.handleClient();
	MDNS.update();

	// update values from the sensors at 10 seconds pace & send it to the server
	if (millis() - lastTime > 10000) {
		// reset timer
		lastTime = millis();

		// read sensors data
		takeSamples();

		// debug serial data
		#ifdef DEBUG
		serialDebug();
		#endif

		// influx send data
		infxSendData();

		// user feedback
		#ifdef DEBUG
		Serial.println("Sensor data updated & uploaded");
		#endif
	}

	// check if it's time to gone off
	if (millis() > wakeExpireAt) {
		goneAsleep();
	}
}
