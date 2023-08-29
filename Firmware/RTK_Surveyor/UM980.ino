/*
    TODO:
        Add debug menu for direct to USB
        Blink GNSS LED
        Blink BT LED
        Get module version info um980PrintInfo()
*/
void um980Begin()
{
    // We have ID'd the board and GNSS module type, but we have not beginBoard() yet so
    // set the pertinent pins here.

    // Instantiate the library
    um980 = new UM980();
    um980Config = new HardwareSerial(1); // Use UART1 on the ESP32

    pin_UART1_RX = 26;
    pin_UART1_TX = 27;
    pin_GNSS_DR_Reset = 22;

    pinMode(pin_GNSS_DR_Reset, OUTPUT);
    digitalWrite(pin_GNSS_DR_Reset, HIGH); // Tell UM980 and DR to boot

    // We must start the serial port before handing it over to the library
    um980Config->begin(115200, SERIAL_8N1, pin_UART1_RX, pin_UART1_TX);

    // Turn on/off debug messages
    if (settings.enableGNSSdebug)
        um980EnableDebugging();

    if (um980->begin(*um980Config) == false) // Give the serial port over to the library
    {
        log_d("GNSS Failed to begin. Trying again.");

        // Try again with power on delay
        delay(1000);
        if (um980->begin(*um980Config) == false)
        {
            log_d("GNSS offline");
            displayGNSSFail(1000);
            return;
        }
    }
    Serial.println("UM980 detected.");

    // TODO check firmware version and print info

    online.gnss = true;
}

bool um980Configure()
{
    /*
        Disable all message traffic
        Set COM port baud rates,
          UM980 COM1 - Direct to USB, 115200
          UM980 COM2 - To IMU, then to ESP32 for BT. From settings.
          UM980 COM3 - Config and LoRa Radio. Configured for 115200 from begin().
        Set minCNO
        Set elevationAngle
        Set Constellations
        Set messages
          Enable selected NMEA messages on COM2
          Enable selected RTCM messages on COM2
    */

    if (settings.enableGNSSdebug)
        um980->enableDebugging(); // Print all debug to Serial

    // Check if um980Constellations, um980MessageRatesNMEA, um980MessageRatesRTCMRover, um980MessageRatesRTCMBase need to be defaulted
    checkArrayDefaults();

    um980DisableAllOutput();

    bool response = true;
    response &= um980->setPortBaudrate("COM1", 115200);      // Connected to switch, then USB
    response &= um980SetBaudRateCOM2(settings.dataPortBaud); // Conected to ESP UART2

    response &= um980SetMinElevation(settings.minElev); // UM980 default is 5 degrees. Our default is 10.

    response &= um980SetMinCNO(settings.minCNO_um980);

    response &= um980SetConstellations();

    response &= um980EnableNMEA(); // Only turn on messages, do not turn off messages. We assume the caller has UNLOG or
                                   // similar.
    if (response == false)
    {
        systemPrintln("UM980 failed to configure");
    }

    return (response);
}

bool um980ConfigureRover()
{
    /*
        Disable all message traffic
        Cancel any survey-in modes
        Set mode to Rover + dynamic model
        Set minElevation
        Enable RTCM messages on COM2
        Enable NMEA on COM2
    */
    if (online.gnss == false)
    {
        log_d("GNSS not online");
        return (false);
    }

    um980DisableAllOutput();

    bool response = true;

    response &= um980SetModel(settings.dynamicModel); // This will cancel any base averaging mode

    response &= um980SetMinElevation(settings.minElev); // UM980 default is 5 degrees. Our default is 10.

    response &= um980EnableNMEA(); // Only turn on messages, do not turn off messages. We assume the caller has UNLOG or
                                   // similar.

    // TODO consider reducing the GSV setence to 1/4 of the GPGGA setting

    response &= um980EnableRTCMRover(); // Only turn on messages, do not turn off messages. We assume the caller has
                                        // UNLOG or similar.

    response &= um980SaveConfiguration();

    if (response == false)
    {
        systemPrintln("UM980 Rover failed to configure");
    }

    return (response);
}

bool um980ConfigureBase()
{
    /*
        Set all message traffic to 1Hz
        Set GSV NMEA setence to whatever rate the user has selected
        Disable NMEA GGA message
        Disable survey in mode
        Enable RTCM Base messages
    */

    if (online.gnss == false)
    {
        log_d("GNSS not online");
        return (false);
    }

    um980DisableAllOutput();

    bool response = true;

    response &= um980SetMinElevation(settings.minElev); // UM980 default is 5 degrees. Our default is 10.

    response &= um980EnableRTCMBase(); // Only turn on messages, do not turn off messages. We assume the caller has
                                       // UNLOG or similar.

    response &= um980SaveConfiguration();

    if (response == false)
    {
        systemPrintln("UM980 Base failed to configure");
    }

    return (response);
}

// Start a Self-optimizing Base Station
// We do not use the distance parameter (settings.observationPositionAccuracy) because that
// setting on the UM980 is related to automatically restarting base mode
// at power on (very different from ZED-F9P).
bool um980BaseAverageStart()
{
    bool response = true;

    response &=
        um980->setModeBaseAverage(settings.observationSeconds); // Average for a number of seconds (default is 60)

    um980BaseStartTimer = millis(); // Stamp when averaging began

    if (response == false)
    {
        systemPrintln("Survey start failed");
        return (false);
    }

    return (response);
}

// Start the base using fixed coordinates
bool um980FixedBaseStart()
{
    bool response = true;

    if (settings.fixedBaseCoordinateType == COORD_TYPE_ECEF)
    {
        um980->setModeBaseECEF(settings.fixedEcefX, settings.fixedEcefY, settings.fixedEcefZ);
    }
    else if (settings.fixedBaseCoordinateType == COORD_TYPE_GEODETIC)
    {
        // Add height of instrument (HI) to fixed altitude
        // https://www.e-education.psu.edu/geog862/node/1853
        // For example, if HAE is at 100.0m, + 2m stick + 73mm ARP = 102.073
        float totalFixedAltitude =
            settings.fixedAltitude + (settings.antennaHeight / 1000.0) + (settings.antennaReferencePoint / 1000.0);

        um980->setModeBaseGeodetic(settings.fixedLat, settings.fixedLong, totalFixedAltitude);
    }

    return (response);
}

// Turn on all the enabled NMEA messages on COM2
bool um980EnableNMEA()
{
    bool response = true;

    for (int messageNumber = 0; messageNumber < MAX_UM980_NMEA_MSG; messageNumber++)
    {
        // Only turn on messages, do not turn off messages set to 0. This saves on command sending. We assume the caller
        // has UNLOG or similar.
        if (settings.um980MessageRatesNMEA[messageNumber] > 0)
        {
            if (um980->setNMEAPortMessage(umMessagesNMEA[messageNumber].msgTextName, "COM2",
                                          settings.um980MessageRatesNMEA[messageNumber]) == false)
            {
                log_d("Enable NMEA failed at messageNumber %d %s.", messageNumber,
                      umMessagesNMEA[messageNumber].msgTextName);
                response &= false; // If any one of the commands fails, report failure overall
            }
        }
    }

    return (response);
}

// Turn on all the enabled RTCM Rover messages on COM2
bool um980EnableRTCMRover()
{
    bool response = true;

    for (int messageNumber = 0; messageNumber < MAX_UM980_RTCM_MSG; messageNumber++)
    {
        // Only turn on messages, do not turn off messages set to 0. This saves on command sending. We assume the caller
        // has UNLOG or similar.
        if (settings.um980MessageRatesRTCMRover[messageNumber] > 0)
        {
            if (um980->setRTCMPortMessage(umMessagesRTCM[messageNumber].msgTextName, "COM2",
                                          settings.um980MessageRatesRTCMRover[messageNumber]) == false)
            {
                log_d("Enable RTCM failed at messageNumber %d %s.", messageNumber,
                      umMessagesRTCM[messageNumber].msgTextName);
                response &= false; // If any one of the commands fails, report failure overall
            }
        }
    }

    return (response);
}

// Turn on all the enabled RTCM Base messages on COM2
bool um980EnableRTCMBase()
{
    bool response = true;

    for (int messageNumber = 0; messageNumber < MAX_UM980_RTCM_MSG; messageNumber++)
    {
        // Only turn on messages, do not turn off messages set to 0. This saves on command sending. We assume the caller
        // has UNLOG or similar.
        if (settings.um980MessageRatesRTCMBase[messageNumber] > 0)
        {
            if (um980->setRTCMPortMessage(umMessagesRTCM[messageNumber].msgTextName, "COM2",
                                          settings.um980MessageRatesRTCMBase[messageNumber]) == false)
            {
                log_d("Enable RTCM failed at messageNumber %d %s.", messageNumber,
                      umMessagesRTCM[messageNumber].msgTextName);
                response &= false; // If any one of the commands fails, report failure overall
            }
        }
    }

    return (response);
}

// Turn on all the enabled Constellations
bool um980SetConstellations()
{
    bool response = true;

    for (int constellationNumber = 0; constellationNumber < MAX_UM980_CONSTELLATIONS; constellationNumber++)
    {
        if (settings.um980Constellations[constellationNumber] == true)
        {
            if (um980->enableConstellation(um980ConstellationCommands[constellationNumber].textCommand) == false)
            {
                log_d("Enable constellation failed at constellationNumber %d %s.", constellationNumber,
                      um980ConstellationCommands[constellationNumber].textName);
                response &= false; // If any one of the commands fails, report failure overall
            }
        }
        else
        {
            if (um980->disableConstellation(um980ConstellationCommands[constellationNumber].textCommand) == false)
            {
                log_d("Disable constellation failed at constellationNumber %d %s.", constellationNumber,
                      um980ConstellationCommands[constellationNumber].textName);
                response &= false; // If any one of the commands fails, report failure overall
            }
        }
    }

    return (response);
}

// Turn off all NMEA and RTCM
void um980DisableAllOutput()
{
    // Re-attempt as necessary
    for (int x = 0; x < 3; x++)
    {
        if (um980->sendCommand("UNLOG COM2") == true)
            break;
    }
}

// Disable all output, then re-enable
void um980DisableRTCM()
{
    um980DisableAllOutput();
    um980EnableNMEA();
}

bool um980SetMinElevation(uint8_t elevation)
{
    return (um980->setElevationAngle(elevation));
}

bool um980SetMinCNO(uint8_t minCNO)
{
    return (um980->setMinCNO(minCNO));
}

bool um980SetModel(uint8_t modelNumber)
{
    if (modelNumber == UM980_DYN_MODEL_SURVEY)
        return (um980->setModeRoverSurvey());
    else if (modelNumber == UM980_DYN_MODEL_UAV)
        return (um980->setModeRoverUAV());
    else if (modelNumber == UM980_DYN_MODEL_AUTOMOTIVE)
        return (um980->setModeRoverAutomotive());
    return (false);
}

void um980FactoryReset()
{
    um980->factoryReset();

    //   systemPrintln("Waiting for UM980 to reboot");
    //   while (1)
    //   {
    //     delay(1000); //Wait for device to reboot
    //     if (um980->isConnected() == true) break;
    //     else systemPrintln("Device still rebooting");
    //   }
    //   systemPrintln("UM980 has been factory reset");
}

// The UM980 does not have a rate setting. Instead the report rate of
// the GNSS messages can be set. For example, 0.5 is 2Hz, 0.2 is 5Hz.
// We assume, if the user wants to set the 'rate' to 5Hz, they want all
// messages set to that rate.
// All NMEA/RTCM for a rover will be based on the measurementRate setting
// ie, if a message != 0, then it will be output at the measurementRate.
// All RTCM for a base will be based on a measurementRate of 1 with messages
// that can be reported more slowly than that (ie 1 per 10 seconds).
bool um980SetRate(double secondsBetweenSolutions)
{
    bool response = true;

    um980DisableAllOutput();

    // Overwrite any enabled messages with this rate
    for (int messageNumber = 0; messageNumber < MAX_UM980_NMEA_MSG; messageNumber++)
    {
        if (settings.um980MessageRatesNMEA[messageNumber] > 0)
        {
            settings.um980MessageRatesNMEA[messageNumber] = secondsBetweenSolutions;
        }
    }
    response &= um980EnableNMEA(); // Enact these rates

    // TODO we don't know what state we are in, so we don't
    // know which RTCM settings to update. Assume we are
    // in rover for now
    for (int messageNumber = 0; messageNumber < MAX_UM980_RTCM_MSG; messageNumber++)
    {
        if (settings.um980MessageRatesRTCMRover[messageNumber] > 0)
        {
            settings.um980MessageRatesRTCMRover[messageNumber] = secondsBetweenSolutions;
        }
    }
    response &= um980EnableRTCMRover(); // Enact these rates

    return (response);
}

// Send data direct from ESP UART2 to UM980 UART2
// Note: The Tilt sensor is inbetween and may be affected.
int um980PushRawData(uint8_t *dataToSend, int dataLength)
{
    return (serialGNSS->write(dataToSend, dataLength));
}

// Set the baud rate of the UM980 com port 2
// This is used during the Bluetooth test
bool um980SetBaudRateCOM2(uint32_t baudRate)
{
    bool response = true;

    response &= um980->setPortBaudrate("COM2", baudRate);

    return (response);
}

// Return the lower of the two Lat/Long deviations
float um980GetHorizontalAccuracy()
{
    float latitudeDeviation = um980->getLatitudeDeviation();
    float longitudeDeviation = um980->getLongitudeDeviation();

    if (longitudeDeviation < latitudeDeviation)
        return (longitudeDeviation);

    return (latitudeDeviation);
}

int um980GetSatellitesInView()
{
    return (um980->getSIV());
}

double um980GetLatitude()
{
    return (um980->getLatitude());
}

double um980GetLongitude()
{
    return (um980->getLongitude());
}

double um980GetAltitude()
{
    return (um980->getAltitude());
}

bool um980IsValidTime()
{
    if (um980->getTimeStatus() == 0) // 0 = valid, 3 = invalid
        return (true);
    return (false);
}

bool um980IsValidDate()
{
    if (um980->getDateStatus() == 1) // 0 = Invalid, 1 = valid, 2 = leap second warning
        return (true);
    return (false);
}

uint8_t um980GetSolutionStatus()
{
    return (um980->getSolutionStatus()); // 0 = None, 1 = FixedPos, 8 = DopplerVelocity, 16 = Single, ...
}

bool um980IsFullyResolved()
{
    // UM980 does not have this feature directly.
    // getSolutionStatus: 0 = None, 1 = FixedPos, 8 = DopplerVelocity, 16 = Single, ...
    if (um980GetSolutionStatus() >= 8)
        return (true);
    return (false);
}

// Standard deviation of the receiver clock offset, s.
// UM980 returns seconds, ZED returns nanoseconds. We convert here to ns.
// Return just ns in uint32_t form
uint32_t um980GetTimeDeviation()
{
    double timeDeviation_s = um980->getTimeOffsetDeviation();
    Serial.printf("um980 timeDeviation_s: %0.10f\r\n", timeDeviation_s);
    if (timeDeviation_s > 1.0)
        return (999999999);

    uint32_t timeDeviation_ns = timeDeviation_s * 1000000000L; // Convert to nanoseconds
    Serial.printf("um980 timeDeviation_ns: %d\r\n", timeDeviation_ns);
    return (timeDeviation_ns);
}

// 0 = None, 1 = FixedPos, 8 = DopplerVelocity, 16 = Single, ...
uint8_t um980GetPositionType()
{
    return (um980->getPositionType());
}

// Return full year, ie 2023, not 23.
uint16_t um980GetYear()
{
    return (um980->getYear());
}
uint8_t um980GetMonth()
{
    return (um980->getMonth());
}
uint8_t um980GetDay()
{
    return (um980->getDay());
}
uint8_t um980GetHour()
{
    return (um980->getHour());
}
uint8_t um980GetMinute()
{
    return (um980->getMinute());
}
uint8_t um980GetSecond()
{
    return (um980->getSecond());
}
uint8_t um980GetMillisecond()
{
    return (um980->getMillisecond());
}

// Print the module type and firmware version
void um980PrintInfo()
{
    systemPrintf("UM980 firmware: %s\r\n", "TODO");
}

// Return the number of milliseconds since data was updated
uint16_t um980FixAgeMilliseconds()
{
    return (um980->getFixAgeMilliseconds());
}

void um980SaveConfiguration()
{
    um980->saveConfiguration();
}

void um980EnableDebugging()
{
  um980->enableDebugging(); // Print all debug to Serial
}
void um980DisableDebugging()
{
  um980->disableDebugging();
}
