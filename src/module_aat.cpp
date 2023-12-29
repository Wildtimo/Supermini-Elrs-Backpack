#if defined(AAT_BACKPACK)
#include "common.h"
#include "module_aat.h"
#include "logging.h"
#include "devWifi.h"

#include <math.h>
#include <Arduino.h>

#define DEG2RAD(deg) ((deg) * M_PI / 180.0)
#define RAD2DEG(rad) ((rad) * 180.0 / M_PI)
#define DELAY_IDLE          (20U)   // sleep used when not tracking
#define DELAY_FIRST_UPDATE  (5000U) // absolute delay before first servo update

static void calcDistAndAzimuth(int32_t srcLat, int32_t srcLon, int32_t dstLat, int32_t dstLon,
    uint32_t *out_dist, uint32_t *out_azimuth)
{
    // https://www.movable-type.co.uk/scripts/latlong.html
    // https://www.igismap.com/formula-to-find-bearing-or-heading-angle-between-two-points-latitude-longitude/

    // Have to use doubles for at least some of these, due to short distances getting rounded
    // particularly cos(deltaLon) for <2000 m rounds to 1.0000000000
    double deltaLon = DEG2RAD((float)(dstLon - srcLon) / 1e7);
    double thetaA = DEG2RAD((float)srcLat / 1e7);
    double thetaB = DEG2RAD((float)dstLat / 1e7);
    double cosThetaA = cos(thetaA);
    double cosThetaB = cos(thetaB);
    double sinThetaA = sin(thetaA);
    double sinThetaB = sin(thetaB);
    double cosDeltaLon = cos(deltaLon);
    double sinDeltaLon = sin(deltaLon);

    if (out_dist)
    {
        const double R = 6371e3;
        double dist = acos(sinThetaA * sinThetaB + cosThetaA * cosThetaB * cosDeltaLon) * R;
        *out_dist = (uint32_t)dist;
    }

    if (out_azimuth)
    {
        double X = cosThetaB * sinDeltaLon;
        double Y = cosThetaA * sinThetaB - sinThetaA * cosThetaB * cosDeltaLon;

        // Convert to degrees, normalized to 0-360
        uint32_t hdg = RAD2DEG(atan2(X, Y));
        *out_azimuth = (hdg + 360) % 360;
    }
}

static int32_t calcElevation(uint32_t distance, int32_t altitude)
{
    return RAD2DEG(atan2(altitude, distance));
}

AatModule::AatModule(Stream &port) :
    CrsfModuleBase(port), _gpsLast{0}, _home{0},
    _gpsAvgUpdateInterval(0), _lastServoUpdateMs(0), _targetDistance(0),
    _targetAzim(0), _targetElev(0), _azimMsPerDegree(0),
    _servoPos{0}
#if defined(PIN_SERVO_AZIM)
    , _servo_Azim()
#endif
#if defined(PIN_SERVO_ELEV)
    , _servo_Elev()
#endif
#if defined(PIN_OLED_SDA)
    , _display(SCREEN_WIDTH, SCREEN_HEIGHT)
#endif
{
  // Init is called manually
}

void AatModule::Init()
{
#if !defined(DEBUG_LOG)
    // Need to call _port's end but it is a stream reference not the HardwareSerial
    Serial.end();
#endif
#if defined(PIN_SERVO_AZIM)
    _servoPos[IDX_AZIM] = (config.GetAatServoLow(IDX_AZIM) + config.GetAatServoHigh(IDX_AZIM)) / 2;
    _servo_Azim.attach(PIN_SERVO_AZIM, 500, 2500, _servoPos[IDX_AZIM]);
    _servoPos[IDX_AZIM] *= 100;
#endif
#if defined(PIN_SERVO_ELEV)
    _servoPos[IDX_ELEV] = (config.GetAatServoLow(IDX_ELEV) + config.GetAatServoHigh(IDX_ELEV)) / 2;
    _servo_Elev.attach(PIN_SERVO_ELEV, 500, 2500, _servoPos[IDX_ELEV]);
    _servoPos[IDX_ELEV] *= 100;
#endif
    displayInit();
    ModuleBase::Init();
}

void AatModule::SendGpsTelemetry(crsf_packet_gps_t *packet)
{
    _gpsLast.lat = be32toh(packet->p.lat);
    _gpsLast.lon = be32toh(packet->p.lon);
    _gpsLast.speed = be16toh(packet->p.speed);
    _gpsLast.heading = be16toh(packet->p.heading);
    _gpsLast.altitude = (int32_t)be16toh(packet->p.altitude) - 1000;
    _gpsLast.satcnt = packet->p.satcnt;

    //DBGLN("GPS: (%d,%d) %dm %usats", _gpsLast.lat, _gpsLast.lon,
    //    _gpsLast.altitude, _gpsLast.satcnt);

    _gpsLast.updated = true;
}

void AatModule::updateGpsInterval(uint32_t interval)
{
    // Avg is in ms * 100
    interval *= 100;
    // Low pass filter. Note there is no fast init of the average, so it will take some time to grow
    // this prevents overprojection caused by the first update after setting home
    _gpsAvgUpdateInterval += ((int32_t)interval - (int32_t)_gpsAvgUpdateInterval) / 4;

    // Limit the maximum interval to provent projecting for too long
    const uint32_t GPS_UPDATE_INTERVAL_MAX = (10U * 1000U * 100U);
    if (_gpsAvgUpdateInterval > GPS_UPDATE_INTERVAL_MAX)
        _gpsAvgUpdateInterval = GPS_UPDATE_INTERVAL_MAX;
}

uint8_t AatModule::calcGpsIntervalPct(uint32_t now)
{
    if (_gpsAvgUpdateInterval)
    {
        return constrain((now - _gpsLast.lastUpdateMs) * (100U * 100U) / (uint32_t)_gpsAvgUpdateInterval, 0U, 100U);
    }

    return 0;
}

void AatModule::processGps(uint32_t now)
{
    if (!_gpsLast.updated)
        return;
    _gpsLast.updated = false;

    // Actually want to track time between _processing_ each GPS update
    uint32_t interval = now - _gpsLast.lastUpdateMs;
    _gpsLast.lastUpdateMs = now;

    // Check if need to set home position
    bool didSetHome = false;
    if (!isHomeSet())
    {
        if (_gpsLast.satcnt >= config.GetAatSatelliteHomeMin())
        {
            didSetHome = true;
            _home.lat = _gpsLast.lat;
            _home.lon = _gpsLast.lon;
            _home.alt = _gpsLast.altitude;
            DBGLN("GPS Home set to (%d,%d)", _home.lat, _home.lon);
        }
        else
            return;
    }

    uint32_t azimuth;
    uint32_t distance;
    calcDistAndAzimuth(_home.lat, _home.lon, _gpsLast.lat, _gpsLast.lon, &distance, &azimuth);
    uint8_t elevation = constrain(calcElevation(distance, _gpsLast.altitude - _home.alt), 0, 90);
    DBGLN("Azimuth: %udeg Elevation: %udeg Distance: %um", azimuth, elevation, distance);

    // Calculate angular velocity to allow dead reckoning projection
    if (!didSetHome)
    {
        updateGpsInterval(interval);
        // azimDelta is the azimuth change since the last packet, -180 to +180
        int32_t azimDelta = (azimuth - _targetAzim + 540) % 360 - 180;
        _azimMsPerDegree = (azimDelta == 0) ? 0 : (int32_t)interval / azimDelta;
        DBGLN("%d delta in %ums, %dms/d %uavg", azimDelta, interval, _azimMsPerDegree, _gpsAvgUpdateInterval);
    }

    _targetDistance = distance;
    _targetElev = elevation;
    _targetAzim = azimuth;
}

int32_t AatModule::calcProjectedAzim(uint32_t now)
{
    // Attempt to do a linear projection of the last
    // If enabled, we know the GPS update rate, the azimuth has changed, and more than a few meters away
    if (config.GetAatProject() && _gpsAvgUpdateInterval && _azimMsPerDegree && _targetDistance > 3)
    {
        uint32_t elapsed = constrain(now - _gpsLast.lastUpdateMs, 0U, _gpsAvgUpdateInterval / 100U);

        // Prevent excessive rotational velocity (100 degrees per second / 10ms per degree)
        int32_t azimMsPDLimited;
        if (_azimMsPerDegree > -10 && _azimMsPerDegree < 10)
            if (_azimMsPerDegree > 0)
                azimMsPDLimited = 10;
            else
                azimMsPDLimited = -10;
        else
            azimMsPDLimited = _azimMsPerDegree;

        int32_t target = (((int32_t)elapsed / azimMsPDLimited) + _targetAzim + 360) % 360;
        //DBGLN("%u t=%d p=%d", elapsed, _targetAzim, target);
        return target;
    }

    return _targetAzim;
}

void AatModule::displayInit()
{
#if defined(PIN_OLED_SDA)
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    _display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    _display.setTextSize(2);
    _display.setTextColor(SSD1306_WHITE);
    _display.setCursor(0, 0);
    if (connectionState == binding)
        _display.print("Bind\nmode...\n\n");
    else
        _display.print("AAT\nBackpack\n\n");
    _display.setTextSize(1);
    _display.print(VERSION);
    _display.display();
#endif
}

void AatModule::displayIdle(uint32_t now)
{
#if defined(PIN_OLED_SDA)
    // A screen with just the GPS position, sat count, and interval bar
    _display.clearDisplay();
    _display.setCursor(0, 0);

    _display.setTextSize(2);
    _display.printf("Sats: %u\n",  _gpsLast.satcnt);

    _display.setTextSize(1);
    _display.printf("\nLat: %d.%07d\nLon: %d.%07d",
        _gpsLast.lat / 10000000, abs(_gpsLast.lat) % 10000000,
        _gpsLast.lon / 10000000, abs(_gpsLast.lon) % 10000000
    );

    displayGpsIntervalBar(now);
    _display.display();
#endif
}

void AatModule::displayActive(uint32_t now, int32_t projectedAzim)
{
#if defined(PIN_OLED_SDA)
    // El:[deg] [alt]m
    // Az:[deg] [dist]
    // Se:[servo elev]us
    // Sa:[servo azim]us
    // With interval bar
    _display.clearDisplay();
    _display.setTextSize(2);
    _display.setCursor(0, 0)
    ;
    _display.printf("El:%02u %dm\nAz:%03u ", _targetElev,
        constrain(_gpsLast.altitude - _home.alt, -99, 999),
        projectedAzim);

    // Target distance has variable width/height but all fits in 3x1 (doublesized) characters
    if (_targetDistance > 999)
    {
        _display.setTextSize(1);
        _display.printf("%u.%03u\nkm\n", _targetDistance / 1000, (_targetDistance % 1000)); // X.XXX km small font
        _display.setTextSize(2);
    }
    else if (_targetDistance > 99)
    {
        _display.printf("%u\n", _targetDistance); // XXX
    }
    else
    {
        _display.printf("%um\n", _targetDistance); // XXm
    }

    _display.printf("Se:%4dus\nSa:%4dus\n", _servoPos[IDX_ELEV]/100, _servoPos[IDX_AZIM]/100);
    displayGpsIntervalBar(now);
    _display.display();
#endif
}

void AatModule::displayGpsIntervalBar(uint32_t now)
{
#if defined(PIN_OLED_SDA)
    if (_gpsAvgUpdateInterval)
    {
        uint8_t gpsIntervalPct = calcGpsIntervalPct(now);
        uint8_t pxHeight = SCREEN_HEIGHT * (100U - gpsIntervalPct) / 100U;
        _display.fillRect(SCREEN_WIDTH - 3, SCREEN_HEIGHT - pxHeight, 2, pxHeight, SSD1306_WHITE);
    }
#endif
}

void AatModule::servoUpdate(uint32_t now)
{
    uint32_t interval = now - _lastServoUpdateMs;
    if (interval < 20U)
        return;
    _lastServoUpdateMs = now;

    int32_t projectedAzim = calcProjectedAzim(now);
    int32_t transformedAzim = projectedAzim;
    int32_t transformedElev = _targetElev;

    // For 1:2 gearing on the azim servo to allow 360 rotation
    // For Elev servos that only go 0-90 and the azim does 360
    transformedAzim = (transformedAzim + 180) % 360; // convert so 0 maps to 1500us
    int32_t newServoPos[IDX_COUNT];
    newServoPos[IDX_AZIM] = 100 * map(transformedAzim, 0, 360, config.GetAatServoLow(IDX_AZIM), config.GetAatServoHigh(IDX_AZIM));
    newServoPos[IDX_ELEV] = 100 * map(transformedElev, 0, 90, config.GetAatServoLow(IDX_ELEV), config.GetAatServoHigh(IDX_ELEV));

    for (uint32_t idx=IDX_AZIM; idx<IDX_COUNT; ++idx)
    {
        int32_t range = 100 * (config.GetAatServoHigh(idx) - config.GetAatServoLow(idx));
        int32_t diff = newServoPos[idx] - _servoPos[idx];
        // If the distance the servo needs to go is more than 80% away
        // jump immediately. otherwise smooth it
        if (abs(diff) * 100 / range > 80)
            _servoPos[idx] = newServoPos[idx];
        else
            _servoPos[idx] += diff / (config.GetAatServoSmooth() + 1);
    }
    //DBGLN("t=%u pro=%d us=%d smoo=%d", _targetAzim, projectedAzim, newServoPos[IDX_AZIM]/100, _servoPos[IDX_AZIM]/100);

#if defined(PIN_SERVO_AZIM)
    _servo_Azim.writeMicroseconds(_servoPos[IDX_AZIM]/100);
#endif
#if defined(PIN_SERVO_ELEV)
    _servo_Elev.writeMicroseconds(_servoPos[IDX_ELEV]/100);
#endif

    displayActive(now, projectedAzim);
}

void AatModule::onCrsfPacketIn(const crsf_header_t *pkt)
{
    if (pkt->sync_byte == CRSF_SYNC_BYTE)
    {
        if (pkt->type == CRSF_FRAMETYPE_GPS)
            SendGpsTelemetry((crsf_packet_gps_t *)pkt);
    }
}

void AatModule::Loop(uint32_t now)
{
    processGps(now);

    if (isHomeSet() && now > DELAY_FIRST_UPDATE)
    {
        servoUpdate(now);
    }
    else
    {
        if (isGpsActive())
            displayIdle(now);
        delay(DELAY_IDLE);
    }

    CrsfModuleBase::Loop(now);
}
#endif /* AAT_BACKPACK */