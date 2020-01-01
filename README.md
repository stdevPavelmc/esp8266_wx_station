# DIY meteo station

For fun and learn, it uses a ESP8266 (NodeMCU) and a ProMini arduinos linked via I2C, the code for the ProMini is on the [ProMini_wx_station](https://github.com/stdevPavelmc/promini_wx_station)

This is an early stage and design is evolving...

## NOTE

This project is a Platformio one, if you use arduino simply copy the files in the `src` folder and change the extensions to .ino and remove the `#include <Arduino.h>` on top of the main.cpp, then look for the following libs and load them in your arduino.

## Libraries

This project use (as you may guess) some libs from others, you can fin them here:

- [InfluxDb](https://github.com/tobiasschuerg/ESP8266_Influx_DB)
- [SFE_BMP180](https://github.com/LowPowerLab/SFE_BMP180)
- [DHTesp](https://github.com/beegee-tokyo/DHTesp)
- [BH1750FVI](https://github.com/enjoyneering/BH1750FVI)
