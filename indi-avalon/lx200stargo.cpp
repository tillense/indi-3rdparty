/*
    Avalon StarGo driver

    Copyright (C) 2019 Christopher Contaxis, Wolfgang Reissenberger,
    Ken Self and Tonino Tasselli

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "lx200stargo.h"

#include "lx200stargofocuser.h"

#include <cmath>
#include <memory>
#include <cstring>
#include <unistd.h>
#ifndef _WIN32
#include <termios.h>
#endif
#include <libnova/julian_day.h>
#include <libnova/sidereal_time.h>

#include "config.h"

const char *RA_DEC_TAB = "RA / DEC";

static class Loader
{
    private:
        std::unique_ptr<LX200StarGo> telescope;
        std::unique_ptr<LX200StarGoFocuser> focuserAux1;

    public:
        Loader()
        {
            telescope.reset(new LX200StarGo());
            // Hint: focuserAux1 is intentionally NOT initialized, since it is a sub device
            //       of LX200StarGo and can be activated and deactivated from the mount controls.
        }

        LX200StarGoFocuser *getFocuserAux1()
        {
            activateFocuserAux1(true);
            return focuserAux1.get();
        }
        // we need to clear it if the AUX1 focuser is disabled in order to remove the device being visible
        void activateFocuserAux1(bool activate)
        {
            if (activate == true && focuserAux1.get() == nullptr)
                focuserAux1.reset(new LX200StarGoFocuser(telescope.get(), "AUX1 Focuser"));
            else if (activate == false)
                focuserAux1.reset();
        }
        // is the AUX1 focuser activated?
        bool isFocuserAux1Activated()
        {
            return (focuserAux1.get() != nullptr);
        }
} loader;

/**************************************************
*** LX200 Generic Implementation
***************************************************/

LX200StarGo::LX200StarGo()
{
    LOG_DEBUG(__FUNCTION__);
    setVersion(AVALON_VERSION_MAJOR, AVALON_VERSION_MINOR);

    DBG_SCOPE = INDI::Logger::DBG_DEBUG;

    /* missing capabilities
     * TELESCOPE_HAS_TIME:
     *    missing commands - values can be set but not read
     *      :GG# (Get UTC offset time)
     *      :GL# (Get Local Time in 24 hour format)
     *
     * LX200_HAS_ALIGNMENT_TYPE
     *     missing commands
     *        ACK - Alignment Query or GW
     *
     * LX200_HAS_SITES
     *    Makes no sense in combination with KStars?
     *     missing commands
     *        :GM# (Get Site 1 Name)
     *
     * LX200_HAS_TRACKING_FREQ
     *     missing commands
     *        :GT# (Get tracking rate) - doesn't work with StarGo
     *
     * untested, hence disabled:
     * LX200_HAS_FOCUS
     */

    setLX200Capability(LX200_HAS_PULSE_GUIDING );

    SetTelescopeCapability(TELESCOPE_CAN_PARK | TELESCOPE_CAN_SYNC | TELESCOPE_CAN_GOTO | TELESCOPE_CAN_ABORT |
                           TELESCOPE_HAS_TRACK_MODE | TELESCOPE_HAS_LOCATION | TELESCOPE_CAN_CONTROL_TRACK |
                           TELESCOPE_HAS_PIER_SIDE, 4);
}

/**************************************************************************************
**
***************************************************************************************/
const char *LX200StarGo::getDefaultName()
{
    return "Avalon StarGo";
}


/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::Handshake()
{
    char mountType;
    bool isTracking;
    int alignmentPoints;

    if(!getScopeAlignmentStatus(&mountType, &isTracking, &alignmentPoints))
    {
        LOG_ERROR("Error communication with telescope.");
        return false;
    }

    return true;
}


/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // sync home position
        if (!strcmp(name, SyncHomeSP.name))
        {
            return syncHomePosition();
        }

        // goto home position
        if (!strcmp(name, MountGotoHomeSP.name))
        {
            return slewToHome(states, names, n);
        }
        // parking position
        else if (!strcmp(name, MountSetParkSP.name))
        {
            return setParkPosition(states, names, n);
        }
        // tracking mode
        else if (!strcmp(name, TrackModeSP.name))
        {
            if (IUUpdateSwitch(&TrackModeSP, states, names, n) < 0)
                return false;
            uint8_t trackMode = static_cast<uint8_t>(IUFindOnSwitchIndex(&TrackModeSP));

            bool result = SetTrackMode(trackMode);

            switch (trackMode)
            {
                case TRACK_SIDEREAL:
                    LOG_INFO("Sidereal tracking rate selected.");
                    break;
                case TRACK_SOLAR:
                    LOG_INFO("Solar tracking rate selected.");
                    break;
                case TRACK_LUNAR:
                    LOG_INFO("Lunar tracking rate selected");
                    break;
            }
            TrackModeSP.s = result ? IPS_OK : IPS_ALERT;

            IDSetSwitch(&TrackModeSP, nullptr);
            return result;
        }
        else if (!strcmp(name, ST4StatusSP.name))
        {
            bool enabled = !strcmp(IUFindOnSwitchName(states, names, n), ST4StatusS[INDI_ENABLED].name);
            bool result = setST4Enabled(enabled);

            if(result)
            {
                ST4StatusS[INDI_ENABLED].s = enabled ? ISS_ON : ISS_OFF;
                ST4StatusS[INDI_DISABLED].s = enabled ? ISS_OFF : ISS_ON;
                ST4StatusSP.s = IPS_OK;
            }
            else
            {
                ST4StatusSP.s = IPS_ALERT;
            }
            IDSetSwitch(&ST4StatusSP, nullptr);
            return result;
        }
        else if (!strcmp(name, KeypadStatusSP.name))
        {
            bool enabled = !strcmp(IUFindOnSwitchName(states, names, n), KeypadStatusS[INDI_ENABLED].name);
            bool result = setKeyPadEnabled(enabled);

            if(result)
            {
                KeypadStatusS[INDI_ENABLED].s = enabled ? ISS_ON : ISS_OFF;
                KeypadStatusS[INDI_DISABLED].s = enabled ? ISS_OFF : ISS_ON;
                KeypadStatusSP.s = IPS_OK;
            }
            else
            {
                KeypadStatusSP.s = IPS_ALERT;
            }
            IDSetSwitch(&KeypadStatusSP, nullptr);
            return result;
        }
        else if (!strcmp(name, SystemSpeedSlewSP.name))
        {
            if (IUUpdateSwitch(&SystemSpeedSlewSP, states, names, n) < 0)
                return false;
            int index = IUFindOnSwitchIndex(&SystemSpeedSlewSP);

            bool result = setSystemSlewSpeedMode(index);

            switch (index)
            {
                case 0:
                    LOG_INFO("System slew rate set to low.");
                    break;
                case 1:
                    LOG_INFO("System slew rate set to medium.");
                    break;
                case 2:
                    LOG_INFO("System slew rate set to fast.");
                    break;
                case 3:
                    LOG_WARN("System slew rate set to high. ONLY AVAILABLE FOR 15V or 18V!");
                    break;
                default:
                    LOGF_WARN("Unexpected slew rate %d", index);
                    result = false;
                    break;
            }
            SystemSpeedSlewSP.s = result ? IPS_OK : IPS_ALERT;

            IDSetSwitch(&SystemSpeedSlewSP, nullptr);
            return result;

        }
        else if (!strcmp(name, MeridianFlipModeSP.name))
        {
            int preIndex = IUFindOnSwitchIndex(&MeridianFlipModeSP);
            IUUpdateSwitch(&MeridianFlipModeSP, states, names, n);
            int nowIndex = IUFindOnSwitchIndex(&MeridianFlipModeSP);
            if (SetMeridianFlipMode(nowIndex) == false)
            {
                IUResetSwitch(&MeridianFlipModeSP);
                MeridianFlipModeS[preIndex].s = ISS_ON;
                MeridianFlipModeSP.s          = IPS_ALERT;
            }
            else
                MeridianFlipModeSP.s = IPS_OK;
            IDSetSwitch(&MeridianFlipModeSP, nullptr);
            return true;
        }
        else if (!strcmp(name, Aux1FocuserSP.name))
        {
            if (IUUpdateSwitch(&Aux1FocuserSP, states, names, n) < 0)
                return false;
            bool activated = (IUFindOnSwitchIndex(&Aux1FocuserSP) == DefaultDevice::INDI_ENABLED);
            if (activateFocuserAux1(activated))
            {
                Aux1FocuserSP.s = activated ? IPS_OK : IPS_IDLE;
                IDSetSwitch(&Aux1FocuserSP, nullptr);
                return true;
            }
            else
            {
                Aux1FocuserSP.s = IPS_ALERT;
                IDSetSwitch(&Aux1FocuserSP, nullptr);
                return false;
            }
        }
    }

    bool result = true;
    // check if the focuser can process the switch
    if (loader.isFocuserAux1Activated())
        result = loader.getFocuserAux1()->ISNewSwitch(dev, name, states, names, n);

    //  Pass it to the parent
    result &= LX200Telescope::ISNewSwitch(dev, name, states, names, n);
    return result;
}

bool LX200StarGo::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {

        // sync home position
        if (!strcmp(name, GuidingSpeedNP.name))
        {
            int raSpeed  = static_cast<int>(round(values[0] * 100.0));
            int decSpeed = static_cast<int>(round(values[1] * 100.0));
            bool result  = setGuidingSpeeds(raSpeed, decSpeed);

            if(result)
            {
                GuidingSpeedP[0].value = static_cast<double>(raSpeed) / 100.0;
                GuidingSpeedP[1].value = static_cast<double>(decSpeed) / 100.0;
                GuidingSpeedNP.s = IPS_OK;
            }
            else
            {
                GuidingSpeedNP.s = IPS_ALERT;
            }
            IDSetNumber(&GuidingSpeedNP, nullptr);
            return result;
        }
        else if (!strcmp(name, MountRequestDelayNP.name))
        {
            int secs   = static_cast<int>(floor(values[0] / 1000.0));
            long nsecs = static_cast<long>(round((values[0] - 1000.0 * secs) * 1000000.0));
            setMountRequestDelay(secs, nsecs);

            MountRequestDelayN[0].value = secs * 1000 + nsecs / 1000000;
            MountRequestDelayNP.s = IPS_OK;
            IDSetNumber(&MountRequestDelayNP, nullptr);
            return true;
        }
        else if (!strcmp(name, TrackingAdjustmentNP.name))
        {
            // changing tracking adjustment
            bool success = setTrackingAdjustment(values[0]);
            if (success)
            {
                TrackingAdjustment[0].value = values[0];
                TrackingAdjustmentNP.s      = IPS_OK;
            }
            else
                TrackingAdjustmentNP.s = IPS_ALERT;

            IDSetNumber(&TrackingAdjustmentNP, nullptr);
            return success;
        }
    }

    bool result = true;
    // check if the focuser can process the switch
    if (loader.isFocuserAux1Activated())
        result = loader.getFocuserAux1()->ISNewNumber(dev, name, values, names, n);

    //  Pass it to the parent
    result &= LX200Telescope::ISNewNumber(dev, name, values, names, n);
    return result;
}



/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::initProperties()
{
    /* Make sure to init parent properties first */
    if (!LX200Telescope::initProperties()) return false;

    IUFillSwitch(&Aux1FocuserS[DefaultDevice::INDI_ENABLED], "INDI_ENABLED", "Enabled", ISS_OFF);
    IUFillSwitch(&Aux1FocuserS[DefaultDevice::INDI_DISABLED], "INDI_DISABLED", "Disabled", ISS_ON);
    IUFillSwitchVector(&Aux1FocuserSP, Aux1FocuserS, 2, getDeviceName(), "AUX1_FOCUSER_CONTROL", "AUX1 Focuser",
                       MAIN_CONTROL_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillSwitch(&MountGotoHomeS[0], "MOUNT_GOTO_HOME_VALUE", "Goto Home", ISS_OFF);
    IUFillSwitchVector(&MountGotoHomeSP, MountGotoHomeS, 1, getDeviceName(), "MOUNT_GOTO_HOME", "Goto Home", MAIN_CONTROL_TAB,
                       IP_RW, ISR_ATMOST1, 60, IPS_OK);

    IUFillSwitch(&MountSetParkS[0], "MOUNT_SET_PARK_VALUE", "Set Park", ISS_OFF);
    IUFillSwitchVector(&MountSetParkSP, MountSetParkS, 1, getDeviceName(), "MOUNT_SET_PARK", "Set Park", MAIN_CONTROL_TAB,
                       IP_RW, ISR_ATMOST1, 60, IPS_OK);

    IUFillSwitch(&SyncHomeS[0], "SYNC_HOME", "Sync Home", ISS_OFF);
    IUFillSwitchVector(&SyncHomeSP, SyncHomeS, 1, getDeviceName(), "TELESCOPE_SYNC_HOME", "Home Position", MAIN_CONTROL_TAB,
                       IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    IUFillText(&MountFirmwareInfoT[0], "MOUNT_FIRMWARE_INFO", "Firmware", "");
    IUFillTextVector(&MountFirmwareInfoTP, MountFirmwareInfoT, 1, getDeviceName(), "MOUNT_INFO", "Mount Info", INFO_TAB, IP_RO,
                     60, IPS_OK);

    // Guiding settings
    IUFillNumber(&GuidingSpeedP[0], "GUIDE_RATE_WE", "RA Speed", "%.2f", 0.0, 2.0, 0.1, 0);
    IUFillNumber(&GuidingSpeedP[1], "GUIDE_RATE_NS", "DEC Speed", "%.2f", 0.0, 2.0, 0.1, 0);
    IUFillNumberVector(&GuidingSpeedNP, GuidingSpeedP, 2, getDeviceName(), "GUIDE_RATE", "Autoguiding", RA_DEC_TAB, IP_RW, 60,
                       IPS_IDLE);

    IUFillSwitch(&ST4StatusS[INDI_ENABLED], "INDI_ENABLED", "Enabled", ISS_ON);
    IUFillSwitch(&ST4StatusS[INDI_DISABLED], "INDI_DISABLED", "Disabled", ISS_OFF);
    IUFillSwitchVector(&ST4StatusSP, ST4StatusS, 2, getDeviceName(), "ST4", "ST4", RA_DEC_TAB, IP_RW, ISR_1OFMANY, 60,
                       IPS_IDLE);

    // keypad enabled / disabled
    IUFillSwitch(&KeypadStatusS[INDI_ENABLED], "INDI_ENABLED", "Enabled", ISS_ON);
    IUFillSwitch(&KeypadStatusS[INDI_DISABLED], "INDI_DISABLED", "Disabled", ISS_OFF);
    IUFillSwitchVector(&KeypadStatusSP, KeypadStatusS, 2, getDeviceName(), "Keypad", "Keypad", RA_DEC_TAB, IP_RW, ISR_1OFMANY,
                       60, IPS_IDLE);

    // System speed: Slew
    IUFillSwitch(&SystemSpeedSlewS[0], "SYSTEM_SLEW_SPEED_LOW", "low", ISS_OFF);
    IUFillSwitch(&SystemSpeedSlewS[1], "SYSTEM_SLEW_SPEED_MEDIUM", "medium", ISS_OFF);
    IUFillSwitch(&SystemSpeedSlewS[2], "SYSTEM_SLEW_SPEED_FAST", "fast", ISS_ON);
    IUFillSwitch(&SystemSpeedSlewS[3], "SYSTEM_SLEW_SPEED_HIGH", "high", ISS_OFF);
    IUFillSwitchVector(&SystemSpeedSlewSP, SystemSpeedSlewS, 4, getDeviceName(), "SYSTEM_SLEW_SPEED", "Slew Speed", RA_DEC_TAB,
                       IP_RW, ISR_1OFMANY, 60, IPS_IDLE);

    // Tracking adjustment
    IUFillNumber(&TrackingAdjustment[0], "ADJUSTMENT_RA", "Adj. (max +/- 5%)", "%.2f", -5.0, 5.0, 0.01, 0.0);
    IUFillNumberVector(&TrackingAdjustmentNP, TrackingAdjustment, 1, getDeviceName(), "TRACKING_ADJUSTMENT", "Tracking",
                       RA_DEC_TAB, IP_RW, 60.0, IPS_IDLE);

    // meridian flip
    IUFillSwitch(&MeridianFlipModeS[0], "MERIDIAN_FLIP_AUTO", "auto", ISS_OFF);
    IUFillSwitch(&MeridianFlipModeS[1], "MERIDIAN_FLIP_DISABLED", "disabled", ISS_OFF);
    IUFillSwitch(&MeridianFlipModeS[2], "MERIDIAN_FLIP_FORCED", "forced", ISS_OFF);
    IUFillSwitchVector(&MeridianFlipModeSP, MeridianFlipModeS, 3, getDeviceName(), "MERIDIAN_FLIP_MODE", "Meridian Flip",
                       RA_DEC_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);

    // mount command delay
    IUFillNumber(&MountRequestDelayN[0], "MOUNT_REQUEST_DELAY", "Request Delay (ms)", "%.0f", 0.0, 1000, 1.0, 50.0);
    IUFillNumberVector(&MountRequestDelayNP, MountRequestDelayN, 1, getDeviceName(), "REQUEST_DELAY", "StarGO", RA_DEC_TAB,
                       IP_RW, 60, IPS_OK);

    return true;
}

/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::updateProperties()
{
    if (! LX200Telescope::updateProperties()) return false;
    if (isConnected())
    {
        defineProperty(&Aux1FocuserSP);
        defineProperty(&SyncHomeSP);
        defineProperty(&MountGotoHomeSP);
        defineProperty(&MountSetParkSP);
        defineProperty(&GuidingSpeedNP);
        defineProperty(&ST4StatusSP);
        defineProperty(&KeypadStatusSP);
        defineProperty(&SystemSpeedSlewSP);
        defineProperty(&TrackingAdjustmentNP);
        defineProperty(&MeridianFlipModeSP);
        defineProperty(&MountRequestDelayNP);
        defineProperty(&MountFirmwareInfoTP);
        getStarGoBasicData();
    }
    else
    {
        deleteProperty(Aux1FocuserSP.name);
        deleteProperty(SyncHomeSP.name);
        deleteProperty(MountGotoHomeSP.name);
        deleteProperty(MountSetParkSP.name);
        deleteProperty(GuidingSpeedNP.name);
        deleteProperty(ST4StatusSP.name);
        deleteProperty(KeypadStatusSP.name);
        deleteProperty(TrackingAdjustmentNP.name);
        deleteProperty(SystemSpeedSlewSP.name);
        deleteProperty(MeridianFlipModeSP.name);
        deleteProperty(MountRequestDelayNP.name);
        deleteProperty(MountFirmwareInfoTP.name);
    }

    return true;
}

/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::Connect()
{
    if (! DefaultDevice::Connect())
        return false;

    // activate focuser AUX1 if the switch is set to "activated"
    return activateFocuserAux1((IUFindOnSwitchIndex(&Aux1FocuserSP) == DefaultDevice::INDI_ENABLED));
}

bool LX200StarGo::Disconnect()
{
    bool result = DefaultDevice::Disconnect();
    result &= activateFocuserAux1(false);
    return result;
}

/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::ReadScopeStatus()
{
    if (!isConnected())
        return false;

    if (isSimulation())
    {
        mountSim();
        return true;
    }

    LOG_DEBUG("################################ ReadScopeStatus (start) ################################");
    int x, y;

    if (! getMotorStatus(&x, &y))
    {
        LOG_INFO("Failed to parse motor state. Retrying...");
        // retry once
        if (! getMotorStatus(&x, &y))
        {
            LOG_ERROR("Cannot determine scope status, failed to parse motor state.");
            return false;
        }
    }

    char parkHomeStatus[1] = {0};
    if (! getParkHomeStatus(parkHomeStatus))
    {
        LOG_ERROR("Cannot determine scope status, failed to determine park/sync state.");
        return false;
    }
    LOGF_DEBUG("Mount state = %s", parkHomeStatus);

    INDI::Telescope::TelescopeStatus newTrackState = TrackState;

    // handle parking / unparking
    if(strcmp(parkHomeStatus, "2") == 0)
    {
        newTrackState = SCOPE_PARKED;
        if (TrackState != newTrackState)
            SetParked(true);
    }
    else
    {
        if (TrackState == SCOPE_PARKED)
            SetParked(false);

        // handle tracking state
        if(x == 0 && y == 0)
        {
            newTrackState = SCOPE_IDLE;
            if (TrackState != newTrackState)
                LOGF_INFO("%sTracking is off.", TrackState == SCOPE_PARKING ? "Scope parked. " : "");

            if (MountGotoHomeSP.s == IPS_BUSY)
            {
                MountGotoHomeSP.s = IPS_OK;
                IDSetSwitch(&MountGotoHomeSP, nullptr);
            }
        }
        else if(x == 1 && y == 0)
        {
            newTrackState = SCOPE_TRACKING;  // or GUIDING
            if (TrackState != newTrackState)
                LOGF_INFO("%sTracking...", TrackState == SCOPE_SLEWING ? "Slewing completed. " : "");
        }
    }

    double raCorrection;
    if (getTrackingAdjustment(&raCorrection))
    {
        TrackingAdjustment[0].value = raCorrection;
        TrackingAdjustmentNP.s      = IPS_OK;
    }
    else
        TrackingAdjustmentNP.s = IPS_ALERT;

    IDSetNumber(&TrackingAdjustmentNP, nullptr);

    double r, d;
    if(!getEqCoordinates(&r, &d))
    {
        LOG_ERROR("Retrieving equatorial coordinates failed.");
        return false;
    }
    currentRA = r;
    currentDEC = d;

    TrackState = newTrackState;
    NewRaDec(currentRA, currentDEC);

    if (! syncSideOfPier())
    {
        LOG_ERROR("Cannot determine scope status, failed to determine pier side.");
        return false;
    }

    LOG_DEBUG("################################ ReadScopeStatus (finish) ###############################");

    if (loader.isFocuserAux1Activated() && TrackState != SCOPE_SLEWING)
        return loader.getFocuserAux1()->ReadFocuserStatus();
    else
        return true;
}

/**************************************************************************************
**
***************************************************************************************/

bool LX200StarGo::syncHomePosition()
{
    LOG_DEBUG(__FUNCTION__);
    char input[AVALON_RESPONSE_BUFFER_LENGTH - 5];
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    if (!getLST_String(input))
    {
        LOG_WARN("Synching home get LST failed.");
        SyncHomeSP.s = IPS_ALERT;
        return false;
    }

    sprintf(cmd, ":X31%s#", input);
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    if (sendQuery(cmd, response))
    {
        LOG_INFO("Synching home position succeeded.");
        SyncHomeSP.s = IPS_OK;
    }
    else
    {
        LOG_WARN("Synching home position failed.");
        SyncHomeSP.s = IPS_ALERT;
        return false;
    }
    IDSetSwitch(&SyncHomeSP, nullptr);
    return true;
}

bool LX200StarGo::getEqCoordinates (double *ra, double *dec)
{
    LOG_DEBUG(__FUNCTION__);
    // Use X590 for RA DEC
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(":X590#", response))
    {
        LOGF_ERROR("Unable to get RA and DEC %s", response);
        return false;
    }
    double r, d;
    int returnCode = sscanf(response, "RD%08lf%08lf", &r, &d);
    if (returnCode < 2)
    {
        LOGF_ERROR("Failed to parse RA and Dec response '%s'.", response);
        return false;
    }
    *ra  = r / 1.0e6;
    *dec = d / 1.0e5;

    return true;
}

/**************************************************************************************
* @author CanisUrsa
***************************************************************************************/

bool LX200StarGo::slewToHome(ISState* states, char* names[], int n)
{
    LOG_DEBUG(__FUNCTION__);
    IUUpdateSwitch(&MountGotoHomeSP, states, names, n);
    if (setMountGotoHome())
    {
        MountGotoHomeSP.s = IPS_BUSY;
        TrackState = SCOPE_SLEWING;
    }
    else
    {
        MountGotoHomeSP.s = IPS_ALERT;
    }
    MountGotoHomeS[0].s = ISS_OFF;
    IDSetSwitch(&MountGotoHomeSP, nullptr);

    LOG_INFO("Slewing to home position...");
    return true;
}


bool LX200StarGo::setParkPosition(ISState* states, char* names[], int n)
{
    LOG_DEBUG(__FUNCTION__);
    IUUpdateSwitch(&MountSetParkSP, states, names, n);
    MountSetParkSP.s = setMountParkPosition() ? IPS_OK : IPS_ALERT;
    MountSetParkS[0].s = ISS_OFF;
    IDSetSwitch(&MountSetParkSP, nullptr);
    return true;
}

/**************************************************************************************
**
***************************************************************************************/

void LX200StarGo::getBasicData()
{
    LOG_DEBUG(__FUNCTION__);
    if (!isSimulation())
    {
        checkLX200EquatorialFormat();

        if (genericCapability & LX200_HAS_ALIGNMENT_TYPE)
            getAlignment();

        if (genericCapability & LX200_HAS_TRACKING_FREQ)
        {
            if (! getTrackFrequency(&TrackFreqN[0].value))
                LOG_ERROR("Failed to get tracking frequency from device.");
            else
                IDSetNumber(&TrackFreqNP, nullptr);
        }
    }
}

void LX200StarGo::getStarGoBasicData()
{
    LOG_DEBUG(__FUNCTION__);
    if (!isSimulation())
    {
        MountFirmwareInfoT[0].text = new char[64];
        if (!getFirmwareInfo(MountFirmwareInfoT[0].text))
            LOG_ERROR("Failed to get firmware from device.");
        else
            IDSetText(&MountFirmwareInfoTP, nullptr);

        char parkHomeStatus[1] = {0};
        if (getParkHomeStatus(parkHomeStatus))
        {
            SetParked(strcmp(parkHomeStatus, "2") == 0);
            if (strcmp(parkHomeStatus, "1") == 0)
            {
                SyncHomeS[0].s = ISS_ON;
                SyncHomeSP.s = IPS_OK;
                IDSetSwitch(&SyncHomeSP, nullptr);
            }
        }
        bool isEnabled;
        if (getST4Status(&isEnabled))
        {
            ST4StatusS[INDI_ENABLED].s = isEnabled ? ISS_ON : ISS_OFF;
            ST4StatusS[INDI_DISABLED].s = isEnabled ? ISS_OFF : ISS_ON;
            ST4StatusSP.s = IPS_OK;
        }
        else
        {
            ST4StatusSP.s = IPS_ALERT;
        }
        IDSetSwitch(&ST4StatusSP, nullptr);

        double raCorrection;
        if (getTrackingAdjustment(&raCorrection))
        {
            TrackingAdjustment[0].value = raCorrection;
            TrackingAdjustmentNP.s      = IPS_OK;
        }
        else
            TrackingAdjustmentNP.s = IPS_ALERT;

        IDSetNumber(&TrackingAdjustmentNP, nullptr);

        if (getKeypadStatus(&isEnabled))
        {
            KeypadStatusS[INDI_ENABLED].s = isEnabled ? ISS_ON : ISS_OFF;
            KeypadStatusS[INDI_DISABLED].s = isEnabled ? ISS_OFF : ISS_ON;
            KeypadStatusSP.s = IPS_OK;
        }
        else
        {
            KeypadStatusSP.s = IPS_ALERT;
        }
        IDSetSwitch(&ST4StatusSP, nullptr);

        int index;
        if (GetMeridianFlipMode(&index))
        {
            IUResetSwitch(&MeridianFlipModeSP);
            MeridianFlipModeS[index].s = ISS_ON;
            MeridianFlipModeSP.s   = IPS_OK;
        }
        else
        {
            MeridianFlipModeSP.s = IPS_ALERT;
        }
        IDSetSwitch(&MeridianFlipModeSP, nullptr);

        if (getSystemSlewSpeedMode(&index))
        {
            IUResetSwitch(&SystemSpeedSlewSP);
            SystemSpeedSlewS[index].s = ISS_ON;
            SystemSpeedSlewSP.s   = IPS_OK;
            IDSetSwitch(&SystemSpeedSlewSP, nullptr);
        }
        else
        {
            SystemSpeedSlewSP.s = IPS_ALERT;
        }
        IDSetSwitch(&SystemSpeedSlewSP, nullptr);

        int raSpeed, decSpeed;
        if (getGuidingSpeeds(&raSpeed, &decSpeed))
        {
            GuidingSpeedP[0].value = static_cast<double>(raSpeed) / 100.0;
            GuidingSpeedP[1].value = static_cast<double>(decSpeed) / 100.0;
            GuidingSpeedNP.s = IPS_OK;
        }
        else
        {
            GuidingSpeedNP.s = IPS_ALERT;
        }
        IDSetNumber(&GuidingSpeedNP, nullptr);
    }


    LOGF_DEBUG("sendLocation %s && %s", sendLocationOnStartup ? "T" : "F",
               (GetTelescopeCapability() & TELESCOPE_HAS_LOCATION) ? "T" : "F");
    if (sendLocationOnStartup && (GetTelescopeCapability() & TELESCOPE_HAS_LOCATION))
        sendScopeLocation();

    LOGF_DEBUG("sendTime %s && %s", sendTimeOnStartup ? "T" : "F",
               (GetTelescopeCapability() & TELESCOPE_HAS_TIME) ? "T" : "F");
    if (sendTimeOnStartup && (GetTelescopeCapability() & TELESCOPE_HAS_TIME))
        sendScopeTime();
    //FIXME collect othr fixed data here like Manufacturer, version etc...
    if (genericCapability & LX200_HAS_PULSE_GUIDING)
        usePulseCommand = true;
}

bool LX200StarGo::activateFocuserAux1(bool activate)
{
    if (activate == true)
    {
        loader.activateFocuserAux1(true);
        return loader.getFocuserAux1()->activate(true);
    }
    else
    {
        bool result = true;
        if (loader.isFocuserAux1Activated())
            result = loader.getFocuserAux1()->activate(false);
        loader.activateFocuserAux1(false);
        return result;
    }
}

/**************************************************************************************
* @author CanisUrsa
***************************************************************************************/
bool LX200StarGo::setMountGotoHome()
{
    LOG_DEBUG(__FUNCTION__);
    // Command  - :X361#
    // Response - pA#
    //            :Z1303#
    //            p0#
    //            :Z1003#
    //            p0#
    char response[AVALON_COMMAND_BUFFER_LENGTH] = {0};
    if (!sendQuery(":X361#", response))
    {
        LOG_ERROR("Failed to send mount goto home command.");
        return false;
    }
    if (strcmp(response, "pA") != 0)
    {
        LOGF_ERROR("Invalid send mount goto home response '%s'.", response);
        return false;
    }
    return true;
}

/**************************************************************************************
**
***************************************************************************************/
bool LX200StarGo::sendScopeLocation()
{
    LOG_DEBUG(__FUNCTION__);
    if (isSimulation())
    {
        return LX200Telescope::sendScopeLocation();
    }

    double siteLat = 0.0, siteLong = 0.0;
    if (!getSiteLatitude(&siteLat))
    {
        LOG_WARN("Failed to get site latitude from device.");
        return false;
    }
    if (!getSiteLongitude(&siteLong))
    {
        LOG_WARN("Failed to get site longitude from device.");
        return false;
    }
    LocationNP.np[LOCATION_LATITUDE].value = siteLat;
    LocationNP.np[LOCATION_LONGITUDE].value = siteLong;

    LOGF_DEBUG("Mount Controller Latitude: %lg Longitude: %lg", LocationN[LOCATION_LATITUDE].value,
               LocationN[LOCATION_LONGITUDE].value);

    IDSetNumber(&LocationNP, nullptr);
    if(!setLocalSiderealTime(siteLong))
    {
        LOG_ERROR("Error setting local sidereal time");
        return false;
    }

    return true;
}

/**************************************************************************************
**
***************************************************************************************/

bool LX200StarGo::updateLocation(double latitude, double longitude, double elevation)
{
    LOGF_DEBUG("%s Lat:%.3lf Lon:%.3lf", __FUNCTION__, latitude, longitude);
    INDI_UNUSED(elevation);

    if (isSimulation())
        return true;

    //    LOGF_DEBUG("Setting site longitude '%lf'", longitude);
    if (!isSimulation() && ! setSiteLongitude(longitude))
    {
        LOGF_ERROR("Error setting site longitude %lf", longitude);
        return false;
    }

    if (!isSimulation() && ! setSiteLatitude(latitude))
    {
        LOGF_ERROR("Error setting site latitude %lf", latitude);
        return false;
    }

    char l[32] = {0}, L[32] = {0};
    fs_sexa(l, latitude, 3, 3600);
    fs_sexa(L, longitude, 4, 3600);

    //    LOGF_INFO("Site location updated to Lat %.32s - Long %.32s", l, L);
    if(!setLocalSiderealTime(longitude))
    {
        LOG_ERROR("Error setting local sidereal time");
        return false;
    }
    return true;
}

bool LX200StarGo::setLocalSiderealTime(double longitude)
{
    double lst = get_local_sidereal_time(longitude);
    LOGF_DEBUG("Current local sidereal time = %lf", lst);
    int h = 0, m = 0, s = 0;
    getSexComponents(lst, &h, &m, &s);

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    char cmd[AVALON_COMMAND_BUFFER_LENGTH] = {0};
    sprintf(cmd, ":X32%02hd%02hd%02hd#",
            static_cast<int16_t>(h),
            static_cast<int16_t>(m),
            static_cast<int16_t>(s));

    if(!sendQuery(cmd, response))
    {
        LOG_ERROR("Failed to set LST");
        return false;
    }
    return true;
}

/*
 * Determine the site latitude. In contrast to a standard LX200 implementation,
 * StarGo returns the location in arc seconds precision.
 */

bool LX200StarGo::getSiteLatitude(double *siteLat)
{
    LOG_DEBUG(__FUNCTION__);
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":Gt#", response))
    {
        LOG_ERROR("Failed to send query get Site Latitude command.");
        return false;
    }
    if (f_scansexa(response, siteLat))
    {
        LOGF_ERROR("Unable to parse get Site Latitude response %s", response);
        return false;
    }
    return true;
}

/*
 * Determine the site longitude. In contrast to a standard LX200 implementation,
 * StarGo returns the location in arc seconds precision.
 */

bool LX200StarGo::getSiteLongitude(double *siteLong)
{
    LOG_DEBUG(__FUNCTION__);
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":Gg#", response))
    {
        LOG_ERROR("Failed to send query get Site Longitude command.");
        return false;
    }
    if (f_scansexa(response, siteLong))
    {
        LOG_ERROR("Unable to parse get Site Longitude response.");
        return false;
    }
    return true;
}


/**************************************************************************************
**
***************************************************************************************/

bool LX200StarGo::Park()
{
    LOG_DEBUG(__FUNCTION__);
    // in: :X362#
    // out: "pB#"

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (sendQuery(":X362#", response) && strcmp(response, "pB") == 0)
    {
        LOG_INFO("Parking mount...");
        TrackState = SCOPE_PARKING;
        return true;
    }
    else
    {
        LOGF_ERROR("Parking failed. Response %s", response);
        return false;
    }
}

/**
 * @brief Set parking state to "parked" and reflect the state
 *        in the UI.
 * @param isparked true iff the scope has been parked
 * @return
 */
void LX200StarGo::SetParked(bool isparked)
{
    LOGF_DEBUG("%s %s", __FUNCTION__, isparked ? "PARKED" : "UNPARKED");
    INDI::Telescope::SetParked(isparked);
}

bool LX200StarGo::UnPark()
{
    LOG_DEBUG(__FUNCTION__);
    // in: :X370#
    // out: "p0#"

    double siteLong;

    // step one: determine site longitude
    if (!getSiteLongitude(&siteLong))
    {
        LOG_WARN("Failed to get site Longitude from device.");
        return false;
    }
    // set LST to avoid errors
    if (!setLocalSiderealTime(siteLong))
    {
        LOGF_ERROR("Failed to set LST before unparking %lf", siteLong);
        return false;
    }
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    // and now execute unparking
    if (sendQuery(":X370#", response) && strcmp(response, "p0") == 0)
    {
        LOG_INFO("Unparking mount...");
        return true;
    }
    else
    {
        LOGF_ERROR("Unpark failed with response: %s", response);
        return false;
    }
}

/**
 * @brief Determine the LST with format HHMMSS
 * @return LST value for the current scope locateion
 */

bool LX200StarGo::getLST_String(char* input)
{
    LOG_DEBUG(__FUNCTION__);
    double siteLong;

    // step one: determine site longitude
    if (!getSiteLongitude(&siteLong))
    {
        LOG_WARN("getLST Failed to get site Longitude from device.");
        return false;
    }
    // determine local sidereal time
    double lst = get_local_sidereal_time(siteLong);
    int h = 0, m = 0, s = 0;
    LOGF_DEBUG("Current local sidereal time = %.8lf", lst);
    // translate into hh:mm:ss
    getSexComponents(lst, &h, &m, &s);

    sprintf(input, "%02d%02d%02d", h, m, s);
    return true;
}


/*********************************************************************************
 * config file
 *********************************************************************************/

bool LX200StarGo::saveConfigItems(FILE *fp)
{
    LOG_DEBUG(__FUNCTION__);
    IUSaveConfigText(fp, &SiteNameTP);
    IUSaveConfigSwitch(fp, &Aux1FocuserSP);
    IUSaveConfigNumber(fp, &MountRequestDelayNP);

    if (loader.isFocuserAux1Activated())
        loader.getFocuserAux1()->saveConfigItems(fp);

    return LX200Telescope::saveConfigItems(fp);
}


/*********************************************************************************
 * Queries
 *********************************************************************************/

/**
 * @brief Send a LX200 query to the communication port and read the result.
 * @param cmd LX200 query
 * @param response answer
 * @return true if the command succeeded, false otherwise
 */
bool LX200StarGo::sendQuery(const char* cmd, char* response, char end, int wait)
{
    LOGF_DEBUG("%s %s End:%c Wait:%ds", __FUNCTION__, cmd, end, wait);
    response[0] = '\0';
    char lresponse[AVALON_RESPONSE_BUFFER_LENGTH];
    int lbytes = 0;
    lresponse [0] = '\0';
    while (receive(lresponse, &lbytes, '#', 0))
    {
        lbytes = 0;
        ParseMotionState(lresponse);
        lresponse [0] = '\0';
    }
    flush();
    if(!transmit(cmd))
    {
        LOGF_ERROR("Command <%s> failed.", cmd);
        // sleep for 50 mseconds to avoid flooding the mount with commands
        nanosleep(&mount_request_delay, nullptr);
        return false;
    }
    lresponse[0] = '\0';
    int lwait = wait;
    bool found = false;
    while (receive(lresponse, &lbytes, end, lwait))
    {
        //        LOGF_DEBUG("Found response after %ds %s", lwait, lresponse);
        lbytes = 0;
        if(! ParseMotionState(lresponse))
        {
            // Take the first response that is no motion state
            if (!found)
                strcpy(response, lresponse);
            found = true;
            lwait = 0;
        }
    }
    flush();

    // sleep for 50 mseconds to avoid flooding the mount with commands
    nanosleep(&mount_request_delay, nullptr);

    return true;
}

bool LX200StarGo::ParseMotionState(char* state)
{
    LOGF_DEBUG("%s %s", __FUNCTION__, state);
    int lmotor, lmode, lslew;
    if(sscanf(state, ":Z1%01d%01d%01d", &lmotor, &lmode, &lslew) == 3)
    {
        LOGF_DEBUG("Motion state %s=>Motors: %d, Track: %d, SlewSpeed: %d", state, lmotor, lmode, lslew);
        // m = 0 both motors are OFF (no power)
        // m = 1 RA motor OFF DEC motor ON
        // m = 2 RA motor ON DEC motor OFF
        // m = 3 both motors are ON
        switch(lmotor)
        {
            case 0:
                CurrentMotorsState = MOTORS_OFF;
                break;
            case 1:
                CurrentMotorsState = MOTORS_DEC_ONLY;
                break;
            case 2:
                CurrentMotorsState = MOTORS_RA_ONLY;
                break;
            case 3:
                CurrentMotorsState = MOTORS_ON;
                break;
        };
        // Tracking modes
        // t = 0 no tracking at all
        // t = 1 tracking at moon speed
        // t = 2 tracking at sun speed
        // t = 3 tracking at stars speed (sidereal speed)
        switch(lmode)
        {
            case 0:
                // TRACK_NONE removed, do nothing
                break;
            case 1:
                CurrentTrackMode = TRACK_LUNAR;
                break;
            case 2:
                CurrentTrackMode = TRACK_SOLAR;
                break;
            case 3:
                CurrentTrackMode = TRACK_SIDEREAL;
                break;
        };
        // Slew speed index
        // s = 0 GUIDE speed
        // s = 1 CENTERING speed
        // s = 2 FINDING speed
        // s = 3 MAX speed
        switch(lslew)
        {
            case 0:
                CurrentSlewRate = SLEW_GUIDE;
                break;
            case 1:
                CurrentSlewRate = SLEW_CENTERING;
                break;
            case 2:
                CurrentSlewRate = SLEW_FIND;
                break;
            case 3:
                CurrentSlewRate = SLEW_MAX;
                break;
        };
        return true;
    }
    else
    {
        return false;
    }
}
bool LX200StarGo::setMountParkPosition()
{
    LOG_DEBUG(__FUNCTION__);
    // Command  - :X352#
    // Response - 0#
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":X352#", response))
    {
        LOG_ERROR("Failed to send mount set park position command.");
        return false;
    }
    if (response[0] != '0')
    {
        LOGF_ERROR("Invalid mount set park position response '%s'.", response);
        return false;
    }
    return true;
}

/*
 * Determine the site longitude. In contrast to a standard LX200 implementation,
 * StarGo returns the location in arc seconds precision.
 */
bool LX200StarGo::setSiteLongitude(double longitude)
{
    LOG_DEBUG(__FUNCTION__);
    int d, m, s;
    char command[32] = {0};
    if (longitude > 180) longitude = longitude - 360;
    if (longitude < -180) longitude = 360 + longitude;

    getSexComponents(longitude, &d, &m, &s);

    //    const char* format = ":Sg+%03d*%02d:%02d#";
    //    if (d < 0 || m < 0 || s < 0) format = ":Sg%04d*%02u:%02u#";

    //    snprintf(command, sizeof(command), format, d, m, s);

    if (d < 0 || m < 0 || s < 0)
        snprintf(command, sizeof(command), ":Sg%04d*%02u:%02u#",
                 d,
                 static_cast<uint32_t>(std::abs(m)),
                 static_cast<uint32_t>(std::abs(s)));
    else
        snprintf(command, sizeof(command), ":Sg+%03d*%02d:%02d#", d, m, s);

    LOGF_DEBUG("Sending set site longitude request '%s'", command);

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    bool result = sendQuery(command, response);

    return (result);
}


/**
 * @brief Set the site latitude
 * @param latitude value
 * @return true iff the command succeeded
 */
bool LX200StarGo::setSiteLatitude(double Lat)
{
    LOG_DEBUG(__FUNCTION__);
    int d, m, s;
    char command[32];

    getSexComponents(Lat, &d, &m, &s);

    snprintf(command, sizeof(command), ":St%+03d*%02d:%02d#", d, m, s);

    LOGF_DEBUG("Sending set site latitude request '%s'", command);

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    return (sendQuery(command, response));
}

bool LX200StarGo::getScopeAlignmentStatus(char *mountType, bool *isTracking, int *alignmentPoints)
{
    // Standard LX200 query
    // Returns: <mount><tracking><alignment># where:
    // mount: A-AzEl mounted, P-Equatorially mounted, G-german mounted equatorial tracking: T-tracking, N-not tracking
    // alignment: 0-needs alignment, 1-one star aligned, 2-two star aligned, 3-three star aligned.

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(":GW#", response))
    {
        LOG_ERROR("Error communication with telescope.");
        return false;
    }

    char mt, tracking;
    int nr;
    int returnCode = sscanf(response, "%c%c%01d", &mt, &tracking, &nr);
    if (returnCode < 3)
    {
        LOGF_ERROR("Failed to parse scope alignment status response '%s'.", response);
        return false;
    }

    *mountType = mt;
    *isTracking = (tracking == 'T');
    *alignmentPoints = nr;
    return true;
}

bool LX200StarGo::getMotorStatus(int *xSpeed, int *ySpeed)
{
    // Command  - :X34#
    // the StarGo replies mxy# where x is the RA / AZ motor status and y
    // the DEC / ALT motor status meaning:
    //    x (y) = 0 motor x (y) stopped or unpowered
    //             (use :X3C# if you want  distinguish if stopped or unpowered)
    //    x (y) = 1 motor x (y) returned in tracking mode
    //    x (y) = 2 motor x (y) acelerating
    //    x (y) = 3 motor x (y) decelerating
    //    x (y) = 4 motor x (y) moving at low speed to refine
    //    x (y) = 5 motor x (y) moving at high speed to target

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(":X34#", response))
    {
        LOG_ERROR("Failed to get motor state");
        return false;
    }
    int x, y;
    int returnCode = sscanf(response, "m%01d%01d", &x, &y);
    if (returnCode < 2)
    {
        LOGF_ERROR("Failed to parse motor state response '%s'.", response);
        return false;
    }
    *xSpeed = x;
    *ySpeed = y;
    LOGF_DEBUG("Motor state = (%d, %d)", *xSpeed, *ySpeed);
    return true;
}

/**
 * @brief Check whether the mount is synched or parked.
 * @param status 0=unparked, 1=at home position, 2=parked
 *               A=slewing home, B=slewing to park position
 * @return true if the command succeeded, false otherwise
 */
bool LX200StarGo::getParkHomeStatus (char* status)
{
    LOG_DEBUG(__FUNCTION__);
    // Command   - :X38#
    // Answers:
    // p0 - unparked
    // p1 - at home position
    // p2 - parked
    // pA - slewing home
    // pB - slewing to park position

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":X38#", response))
    {
        LOG_ERROR("Failed to send get parking status request.");
        return false;
    }

    LOGF_DEBUG("%s: response: %s", __FUNCTION__, response);

    if (! sscanf(response, "p%32s[012AB]", status))
    {
        LOGF_ERROR("Unexpected park home status response '%s'.", response);
        return false;
    }

    return true;
}

/**
 * @brief Check if the ST4 port is enabled
 * @param isEnabled - true iff the ST4 port is enabled
 * @return
 */
bool LX200StarGo::getST4Status (bool *isEnabled)
{
    LOG_DEBUG(__FUNCTION__);
    // Command query ST4 status  - :TTGFh#
    //         response enabled  - vh1
    //                  disabled - vh0

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    if (!sendQuery(":TTGFh#", response))
    {
        LOG_ERROR("Failed to send query ST4 status request.");
        return false;
    }
    int answer = 0;
    if (! sscanf(response, "vh%01d", &answer))
    {
        LOGF_ERROR("Unexpected ST4 status response '%s'.", response);
        return false;
    }

    *isEnabled = (answer == 1);
    return true;
}

/**
 * @brief Check if the Keypad port is enabled
 * @param isEnabled - true iff the Keypad port is enabled
 * @return
 */
bool LX200StarGo::getKeypadStatus (bool *isEnabled)
{
    LOG_DEBUG(__FUNCTION__);
    // Command query Keypad status  - :TTGFr#
    //            response enabled  - vh1
    //                     disabled - vh0

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    if (!sendQuery(":TTGFr#", response))
    {
        LOG_ERROR("Failed to send query Keypad status request.");
        return false;
    }
    int answer = 0;
    if (! sscanf(response, "vr%01d", &answer))
    {
        LOGF_ERROR("Unexpected Keypad status response '%s'.", response);
        return false;
    }

    *isEnabled = (answer == 0);
    return true;
}

/**
 * @brief Determine the system slew speed mode
 * @param index - low=0, medium=1, fast=2, high=3
 * @return true iff request succeeded
 */
bool LX200StarGo::getSystemSlewSpeedMode (int *index)
{
    LOG_DEBUG(__FUNCTION__);
    // Command query Keypad status  - :TTGFr#
    //            response enabled  - vh1
    //                     disabled - vh0

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    if (!sendQuery(":TTGMX#", response))
    {
        LOG_ERROR("Failed to send query system slew speed status request.");
        return false;
    }
    int xx = 0, yy = 0;
    if (! sscanf(response, "%02da%02d", &xx, &yy))
    {
        LOGF_ERROR("Unexpected system slew speed status response '%s'.", response);
        return false;
    }

    switch (xx)
    {
        case 6:
            *index = 0;
            break;
        case 8:
            *index = 1;
            break;
        case 9:
            *index = 2;
            break;
        case 12:
            *index = 3;
            break;
        default:
            LOGF_ERROR("Unexpected system slew speed status response '%s'.", response);
            return false;
    }
    return true;
}

bool LX200StarGo::setSystemSlewSpeedMode(int index)
{

    std::string cmd = ":TTMX";
    switch (index)
    {
        case 0:
            cmd.append("0606#");
            break;
        case 1:
            cmd.append("0808#");
            break;
        case 2:
            cmd.append("0909#");
            break;
        case 3:
            cmd.append("1212#");
            break;
        default:
            LOGF_ERROR("Unexpected system slew speed mode '%02d'.", index);
            return false;
    }
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (sendQuery(cmd.c_str(), response))
    {
        return true;
    }
    else
    {
        LOG_ERROR("Setting system slew speed mode FAILED");
        return false;
    }

}


/**
 * @brief Determine the guiding speeds for RA and DEC axis
 * @param raSpeed percentage for RA axis
 * @param decSpeed percenage for DEC axis
 * @return
 */
bool LX200StarGo::getGuidingSpeeds (int *raSpeed, int *decSpeed)
{
    LOG_DEBUG(__FUNCTION__);
    // Command query guiding speeds  - :X22#
    //         response              - rrbdd#
    //         rr RA speed percentage, dd DEC speed percentage

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    if (!sendQuery(":X22#", response))
    {
        LOG_ERROR("Failed to send query guiding speeds request.");
        return false;
    }
    if (! sscanf(response, "%02db%2d", raSpeed, decSpeed))
    {
        LOGF_ERROR("Unexpected guiding speed response '%s'.", response);
        return false;
    }

    return true;
}

/**
 * @brief Set the guiding speeds for RA and DEC axis
 * @param raSpeed percentage for RA axis
 * @param decSpeed percenage for DEC axis
 * @return
 */
bool LX200StarGo::setGuidingSpeeds (int raSpeed, int decSpeed)
{
    LOG_DEBUG(__FUNCTION__);
    // in RA guiding speed  -  :X20rr#
    // in DEC guiding speed - :X21dd#

    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    sprintf(cmd, ":X20%2d#", raSpeed);
    if (sendQuery(cmd, response, 0)) // No response from mount
    {
        LOGF_INFO("Setting RA speed to %2d%%.", raSpeed);
    }
    else
    {
        LOGF_ERROR("Setting RA speed to %2d %% FAILED", raSpeed);
        return false;
    }
    const struct timespec timeout = {0, 100000000L};
    // sleep for 100 mseconds
    nanosleep(&timeout, nullptr);

    sprintf(cmd, ":X21%2d#", decSpeed);
    if (sendQuery(cmd, response, 0))  // No response from mount
    {
        LOGF_INFO("Setting DEC speed to %2d%%.", decSpeed);
    }
    else
    {
        LOGF_ERROR("Setting DEC speed to %2d%% FAILED", decSpeed);
        return false;
    }
    return true;
}

/**
 * @brief Enable or disable the ST4 guiding port
 * @param enabled flag whether enable or disable
 * @return
 */

bool LX200StarGo::setST4Enabled(bool enabled)
{
    LOG_DEBUG(__FUNCTION__);

    const char *cmd = enabled ? ":TTSFh#" : ":TTRFh#";
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (sendQuery(cmd, response))
    {
        LOG_INFO(enabled ? "ST4 port enabled." : "ST4 port disabled.");
        return true;
    }
    else
    {
        LOG_ERROR("Setting ST4 port FAILED");
        return false;
    }
}

bool LX200StarGo::setKeyPadEnabled(bool enabled)
{

    const char *cmd = enabled ? ":TTRFr#" : ":TTSFr#";
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (sendQuery(cmd, response))
    {
        LOG_INFO(enabled ? "Keypad port enabled." : "Keypad port disabled.");
        return true;
    }
    else
    {
        LOG_ERROR("Setting Keypad port FAILED");
        return false;
    }

}

/**
 * @brief Retrieve pier side of the mount and sync it back to the client
 * @return true iff synching succeeds
 */
bool LX200StarGo::syncSideOfPier()
{
    LOG_DEBUG(__FUNCTION__);
    // Command query side of pier - :X39#
    //         side unknown       - PX#
    //         east pointing west - PE#
    //         west pointing east - PW#

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":X39#", response))
    {
        LOG_ERROR("Failed to send query pier side.");
        return false;
    }
    char answer;

    if (! sscanf(response, "P%c", &answer))
    {
        LOGF_ERROR("Unexpected query pier side response '%s'.", response);
        return false;
    }

    switch (answer)
    {
        case 'X':
            LOG_DEBUG("Detected pier side unknown.");
            setPierSide(INDI::Telescope::PIER_UNKNOWN);
            break;
        case 'W':
            LOG_DEBUG("Detected pier side west.");
            setPierSide(INDI::Telescope::PIER_EAST);
            break;
        case 'E':
            LOG_DEBUG("Detected pier side east.");
            setPierSide(INDI::Telescope::PIER_WEST);
            break;
        default:
            break;
    }

    return true;
}

/**
 * @brief Retrieve the firmware info from the mount
 * @param firmwareInfo - firmware description
 * @return
 */
bool LX200StarGo::getFirmwareInfo (char* firmwareInfo)
{
    LOG_DEBUG(__FUNCTION__);
    std::string infoStr;
    char manufacturer[AVALON_RESPONSE_BUFFER_LENGTH] = {0};

    // step 1: retrieve manufacturer
    if (!sendQuery(":GVP#", manufacturer))
    {
        LOG_ERROR("Failed to send get manufacturer request.");
        return false;
    }
    infoStr.assign(manufacturer);

    // step 2: retrieve firmware version
    char firmwareVersion[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":GVN#", firmwareVersion))
    {
        LOG_ERROR("Failed to send get firmware version request.");
        return false;
    }
    infoStr.append(" - ").append(firmwareVersion);

    // step 3: retrieve firmware date
    char firmwareDate[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (!sendQuery(":GVD#", firmwareDate))
    {
        LOG_ERROR("Failed to send get firmware date request.");
        return false;
    }
    std::string dateStr = firmwareDate;
    infoStr.append(" - ").append(dateStr, 1, dateStr.length() - 1);

    strcpy(firmwareInfo, infoStr.c_str());

    return true;
}

/*********************************************************************************
 * Helper functions
 *********************************************************************************/


/**
 * @brief Receive answer from the communication port.
 * @param buffer - buffer holding the answer
 * @param bytes - number of bytes contained in the answer
 * @author CanisUrsa
 * @return true if communication succeeded, false otherwise
 */
bool LX200StarGo::receive(char* buffer, int* bytes, char end, int wait)
{
    //    LOGF_DEBUG("%s timeout=%ds",__FUNCTION__, wait);
    int timeout = wait; //? AVALON_TIMEOUT: 0;
    int returnCode = tty_read_section(PortFD, buffer, end, timeout, bytes);
    if (returnCode != TTY_OK)
    {
        char errorString[MAXRBUF];
        tty_error_msg(returnCode, errorString, MAXRBUF);
        if(returnCode == TTY_TIME_OUT && wait <= 0) return false;
        LOGF_WARN("Failed to receive full response: %s. (Return code: %d)", errorString, returnCode);
        return false;
    }
    if(buffer[*bytes - 1] == '#')
        buffer[*bytes - 1] = '\0'; // remove #
    else
        buffer[*bytes] = '\0';

    return true;
}

/**
 * @brief Flush the communication port.
 * @author CanisUrsa
 */
void LX200StarGo::flush()
{
    //    LOG_DEBUG(__FUNCTION__);
    //    tcflush(PortFD, TCIOFLUSH);
}

bool LX200StarGo::transmit(const char* buffer)
{
    //    LOG_DEBUG(__FUNCTION__);
    int bytesWritten = 0;
    flush();
    int returnCode = tty_write_string(PortFD, buffer, &bytesWritten);

    if (returnCode != TTY_OK)
    {
        char errorString[MAXRBUF];
        tty_error_msg(returnCode, errorString, MAXRBUF);
        LOGF_WARN("Failed to transmit %s. Wrote %d bytes and got error %s.", buffer, bytesWritten, errorString);
        return false;
    }
    return true;
}

bool LX200StarGo::SetTrackMode(uint8_t mode)
{
    LOGF_DEBUG("%s: Set Track Mode %d", __FUNCTION__, mode);
    if (isSimulation())
        return true;

    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];
    char s_mode[10] = {0};

    switch (mode)
    {
        case TRACK_SIDEREAL:
            strcpy(cmd, ":TQ#");
            strcpy(s_mode, "Sidereal");
            break;
        case TRACK_SOLAR:
            strcpy(cmd, ":TS#");
            strcpy(s_mode, "Solar");
            break;
        case TRACK_LUNAR:
            strcpy(cmd, ":TL#");
            strcpy(s_mode, "Lunar");
            break;
        default:
            return false;
    }
    if ( !sendQuery(cmd, response, 0))  // Dont wait for response - there is none
        return false;
    LOGF_INFO("Tracking mode set to %s.", s_mode );

    // Only update tracking frequency if it is defined and not deleted by child classes
    if (genericCapability & LX200_HAS_TRACKING_FREQ)
    {
        LOGF_DEBUG("%s: Get Tracking Freq", __FUNCTION__);
        getTrackFrequency(&TrackFreqN[0].value);
        IDSetNumber(&TrackFreqNP, nullptr);
    }
    return true;
}

bool LX200StarGo::checkLX200EquatorialFormat()
{
    LOG_DEBUG(__FUNCTION__);
    char response[AVALON_RESPONSE_BUFFER_LENGTH];

    controller_format = LX200_LONG_FORMAT;
    //    ::controller_format = LX200_LONG_FORMAT;

    if (!sendQuery(":GR#", response))
    {
        LOG_ERROR("Failed to get RA for format check");
        return false;
    }
    /* If it's short format, try to toggle to high precision format */
    if (strlen(response) <= 5 || response[5] == '.')
    {
        LOG_INFO("Detected low precision format, "
                 "attempting to switch to high precision.");
        if (!sendQuery(":U#", response, 0))
        {
            LOG_ERROR("Failed to switch precision");
            return false;
        }
        if (!sendQuery(":GR#", response))
        {
            LOG_ERROR("Failed to get high precision RA");
            return false;
        }
    }
    if (strlen(response) <= 5 || response[5] == '.')
    {
        controller_format = LX200_SHORT_FORMAT;
        LOG_INFO("Coordinate format is low precision.");
        return 0;

    }
    else if (strlen(response) > 8 && response[8] == '.')
    {
        controller_format = LX200_LONGER_FORMAT;
        LOG_INFO("Coordinate format is ultra high precision.");
        return 0;
    }
    else
    {
        controller_format = LX200_LONG_FORMAT;
        LOG_INFO("Coordinate format is high precision.");
        return 0;
    }
}

bool LX200StarGo::SetSlewRate(int index)
{
    LOG_DEBUG(__FUNCTION__);
    // Convert index to Meade format
    index = 3 - index;

    if (!isSimulation() && !setSlewMode(index))
    {
        SlewRateSP.s = IPS_ALERT;
        IDSetSwitch(&SlewRateSP, "Error setting slew mode.");
        return false;
    }

    SlewRateSP.s = IPS_OK;
    IDSetSwitch(&SlewRateSP, nullptr);
    return true;
}
bool LX200StarGo::setSlewMode(int slewMode)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];

    switch (slewMode)
    {
        case LX200_SLEW_MAX:
            strcpy(cmd, ":RS#");
            break;
        case LX200_SLEW_FIND:
            strcpy(cmd, ":RM#");
            break;
        case LX200_SLEW_CENTER:
            strcpy(cmd, ":RC#");
            break;
        case LX200_SLEW_GUIDE:
            strcpy(cmd, ":RG#");
            break;
        default:
            return false;
    }
    if (!sendQuery(cmd, response, 0)) // Don't wait for response - there isn't one
    {
        return false;
    }
    return true;
}

/*
 * Adjust RA tracking speed.
 */
bool LX200StarGo::setTrackingAdjustment(double adjustRA)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];

    /*
     * :X41sRRR# to adjust the RA tracking speed where s is the sign + or -  and RRR are three digits whose meaning is parts per 10000 of  RA correction .
     * :X43sDDD# to fix the cf DEC offset
     */

    // ensure that -5 <= adjust <= 5
    if (adjustRA > 5.0)
    {
        LOGF_ERROR("Adjusting tracking by %0.2f%% not allowed. Maximal value is 5.0%%", adjustRA);
        return false;
    }
    else if (adjustRA < -5.0)
    {
        LOGF_ERROR("Adjusting tracking by %0.2f%% not allowed. Minimal value is -5.0%%", adjustRA);
        return false;
    }

    int parameter = static_cast<int>(adjustRA * 100);
    sprintf(cmd, ":X41%+04i#", parameter);

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(cmd, response, 0))  // No response
    {
        LOGF_ERROR("Cannot adjust tracking by %d%%", adjustRA);
        return false;
    }
    if (adjustRA == 0.0)
        LOG_INFO("RA tracking adjustment cleared.");
    else
        LOGF_INFO("RA tracking adjustment to %+0.2f%% succeeded.", adjustRA);

    return true;
}


bool LX200StarGo::getTrackingAdjustment(double *valueRA)
{
    /*
     * :X42# to read the tracking adjustment value as orsRRR#
     * :X44# to read the tracking adjustment value as odsDDD#

     */
    LOG_DEBUG(__FUNCTION__);
    int raValue;
    char response[RB_MAX_LEN] = {0};

    if (!sendQuery(":X42#", response))
        return false;

    if (sscanf(response, "or%04d#", &raValue) < 1)
    {
        LOG_ERROR("Unable to parse response");
        return false;
    }

    *valueRA = static_cast<double>(raValue / 100.0);
    return true;
}



bool LX200StarGo::SetMeridianFlipMode(int index)
{
    // 0: Auto mode: Enabled and not Forced
    // 1: Disabled mode: Disabled and not Forced
    // 2: Forced mode: Enabled and Forced
    LOG_DEBUG(__FUNCTION__);

    if (isSimulation())
    {
        MeridianFlipModeSP.s = IPS_OK;
        IDSetSwitch(&MeridianFlipModeSP, nullptr);
        return true;
    }
    if( index > 2)
    {
        LOGF_ERROR("Invalid Meridian Flip Mode %d", index);
        return false;
    }
    const char* enablecmd = index == 1 ? ":TTSFs#" : ":TTRFs#";
    const char* forcecmd  = index == 2 ? ":TTSFd#" : ":TTRFd#";
    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(enablecmd, response) || !sendQuery(forcecmd, response))
    {
        LOGF_ERROR("Cannot set Meridian Flip Mode %d", index);
        return false;
    }

    switch (index)
    {
        case 0:
            LOG_INFO("Meridian flip enabled.");
            break;
        case 1:
            LOG_WARN("Meridian flip DISABLED. BE CAREFUL, THIS MAY CAUSE DAMAGE TO YOUR MOUNT!");
            break;
        case 2:
            LOG_WARN("Meridian flip FORCED. BE CAREFUL, THIS MAY CAUSE DAMAGE TO YOUR MOUNT!");
            break;
    }

    return true;
}
bool LX200StarGo::GetMeridianFlipMode(int* index)
{
    LOG_DEBUG(__FUNCTION__);

    // 0: Auto mode: Enabled and not Forced
    // 1: Disabled mode: Disabled and not Forced
    // 2: Forced mode: Enabled and Forced
    const char* enablecmd = ":TTGFs#";
    const char* forcecmd  = ":TTGFd#";
    char enableresp[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    char forceresp[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if(!sendQuery(enablecmd, enableresp) || !sendQuery(forcecmd, forceresp))
    {
        LOGF_ERROR("Cannot get Meridian Flip Mode %s %s", enableresp, forceresp);
        return false;
    }
    int enable = 0;
    if (! sscanf(enableresp, "vs%01d", &enable))
    {
        LOGF_ERROR("Invalid meridian flip enabled response '%s", enableresp);
        return false;
    }
    int force = 0;
    if (! sscanf(forceresp, "vd%01d", &force))
    {
        LOGF_ERROR("Invalid meridian flip forced response '%s", forceresp);
        return false;
    }

    if( enable == 1)
    {
        *index = 1; // disabled
        LOG_WARN("Meridian flip DISABLED. BE CAREFUL, THIS MAY CAUSE DAMAGE TO YOUR MOUNT!");
    }
    else if( force == 0)
    {
        *index = 0; // auto
        LOG_INFO("Meridian flip enabled.");
    }
    else
    {
        *index = 2; // forced
        LOG_WARN("Meridian flip FORCED. BE CAREFUL, THIS MAY CAUSE DAMAGE TO YOUR MOUNT!");
    }

    return true;

}

IPState LX200StarGo::GuideNorth(uint32_t ms)
{
    LOGF_DEBUG("%s %dms %d", __FUNCTION__, ms, usePulseCommand);
    if (usePulseCommand && (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY))
    {
        LOG_ERROR("Cannot guide while moving.");
        return IPS_ALERT;
    }

    // If already moving (no pulse command), then stop movement
    if (MovementNSSP.s == IPS_BUSY)
    {
        int dir = IUFindOnSwitchIndex(&MovementNSSP);

        MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
    }

    if (GuideNSTID)
    {
        IERmTimer(GuideNSTID);
        GuideNSTID = 0;
    }

    if (usePulseCommand)
    {
        SendPulseCmd(LX200_NORTH, ms);
    }
    else
    {
        if (!setSlewMode(LX200_SLEW_GUIDE))
        {
            SlewRateSP.s = IPS_ALERT;
            IDSetSwitch(&SlewRateSP, "Error setting slew mode.");
            return IPS_ALERT;
        }

        MovementNSS[DIRECTION_NORTH].s = ISS_ON;
        MoveNS(DIRECTION_NORTH, MOTION_START);
    }

    // Set slew to guiding
    IUResetSwitch(&SlewRateSP);
    SlewRateS[SLEW_GUIDE].s = ISS_ON;
    IDSetSwitch(&SlewRateSP, nullptr);
    guide_direction_ns = LX200_NORTH;
    GuideNSTID      = IEAddTimer(static_cast<int>(ms), guideTimeoutHelperNS, this);
    return IPS_BUSY;
}

IPState LX200StarGo::GuideSouth(uint32_t ms)
{
    LOGF_DEBUG("%s %dms %d", __FUNCTION__, ms, usePulseCommand);
    if (usePulseCommand && (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY))
    {
        LOG_ERROR("Cannot guide while moving.");
        return IPS_ALERT;
    }

    // If already moving (no pulse command), then stop movement
    if (MovementNSSP.s == IPS_BUSY)
    {
        int dir = IUFindOnSwitchIndex(&MovementNSSP);

        MoveNS(dir == 0 ? DIRECTION_NORTH : DIRECTION_SOUTH, MOTION_STOP);
    }

    if (GuideNSTID)
    {
        IERmTimer(GuideNSTID);
        GuideNSTID = 0;
    }

    if (usePulseCommand)
    {
        SendPulseCmd(LX200_SOUTH, ms);
    }
    else
    {
        if (!setSlewMode(LX200_SLEW_GUIDE))
        {
            SlewRateSP.s = IPS_ALERT;
            IDSetSwitch(&SlewRateSP, "Error setting slew mode.");
            return IPS_ALERT;
        }

        MovementNSS[DIRECTION_SOUTH].s = ISS_ON;
        MoveNS(DIRECTION_SOUTH, MOTION_START);
    }

    // Set slew to guiding
    IUResetSwitch(&SlewRateSP);
    SlewRateS[SLEW_GUIDE].s = ISS_ON;
    IDSetSwitch(&SlewRateSP, nullptr);
    guide_direction_ns = LX200_SOUTH;
    GuideNSTID         = IEAddTimer(static_cast<int>(ms), guideTimeoutHelperNS, this);
    return IPS_BUSY;
}

IPState LX200StarGo::GuideEast(uint32_t ms)
{
    LOGF_DEBUG("%s %dms %d", __FUNCTION__, ms, usePulseCommand);
    if (usePulseCommand && (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY))
    {
        LOG_ERROR("Cannot guide while moving.");
        return IPS_ALERT;
    }

    // If already moving (no pulse command), then stop movement
    if (MovementWESP.s == IPS_BUSY)
    {
        int dir = IUFindOnSwitchIndex(&MovementWESP);

        MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
    }

    if (GuideWETID)
    {
        IERmTimer(GuideWETID);
        GuideWETID = 0;
    }

    if (usePulseCommand)
    {
        SendPulseCmd(LX200_EAST, ms);
    }
    else
    {
        if (!setSlewMode(LX200_SLEW_GUIDE))
        {
            SlewRateSP.s = IPS_ALERT;
            IDSetSwitch(&SlewRateSP, "Error setting slew mode.");
            return IPS_ALERT;
        }

        MovementWES[DIRECTION_EAST].s = ISS_ON;
        MoveWE(DIRECTION_EAST, MOTION_START);
    }

    // Set slew to guiding
    IUResetSwitch(&SlewRateSP);
    SlewRateS[SLEW_GUIDE].s = ISS_ON;
    IDSetSwitch(&SlewRateSP, nullptr);
    guide_direction_we = LX200_EAST;
    GuideWETID         = IEAddTimer(static_cast<int>(ms), guideTimeoutHelperWE, this);
    return IPS_BUSY;
}

IPState LX200StarGo::GuideWest(uint32_t ms)
{
    LOGF_DEBUG("%s %dms %d", __FUNCTION__, ms, usePulseCommand);
    if (usePulseCommand && (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY))
    {
        LOG_ERROR("Cannot guide while moving.");
        return IPS_ALERT;
    }

    // If already moving (no pulse command), then stop movement
    if (MovementWESP.s == IPS_BUSY)
    {
        int dir = IUFindOnSwitchIndex(&MovementWESP);

        MoveWE(dir == 0 ? DIRECTION_WEST : DIRECTION_EAST, MOTION_STOP);
    }

    if (GuideWETID)
    {
        IERmTimer(GuideWETID);
        GuideWETID = 0;
    }

    if (usePulseCommand)
    {
        SendPulseCmd(LX200_WEST, ms);
    }
    else
    {
        if (!setSlewMode(LX200_SLEW_GUIDE))
        {
            SlewRateSP.s = IPS_ALERT;
            IDSetSwitch(&SlewRateSP, "Error setting slew mode.");
            return IPS_ALERT;
        }

        MovementWES[DIRECTION_WEST].s = ISS_ON;
        MoveWE(DIRECTION_WEST, MOTION_START);
    }

    // Set slew to guiding
    IUResetSwitch(&SlewRateSP);
    SlewRateS[SLEW_GUIDE].s = ISS_ON;
    IDSetSwitch(&SlewRateSP, nullptr);
    guide_direction_we = LX200_WEST;
    GuideWETID         = IEAddTimer(static_cast<int>(ms), guideTimeoutHelperWE, this);
    return IPS_BUSY;
}

int LX200StarGo::SendPulseCmd(int8_t direction, uint32_t duration_msec)
{
    LOGF_DEBUG("%s dir=%d dur=%d ms", __FUNCTION__, direction, duration_msec );

    if (TrackState == SCOPE_SLEWING || TrackState == SCOPE_PARKING)
    {
        // having pulse guiding while slewing or parking creates confusion
        LOGF_INFO("Pulse command (dir=%d dur=%d ms) ingnored due to track state %d.", direction, duration_msec, TrackState);
        return 1;
    }
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];
    switch (direction)
    {
        case LX200_NORTH:
            sprintf(cmd, ":Mgn%04u#", duration_msec);
            break;
        case LX200_SOUTH:
            sprintf(cmd, ":Mgs%04u#", duration_msec);
            break;
        case LX200_EAST:
            sprintf(cmd, ":Mge%04u#", duration_msec);
            break;
        case LX200_WEST:
            sprintf(cmd, ":Mgw%04u#", duration_msec);
            break;
        default:
            return 1;
    }
    bool success = !sendQuery(cmd, response, 0); // no response expected

    return success;
}

bool LX200StarGo::SetTrackEnabled(bool enabled)
{
    LOGF_INFO("Tracking %s.", enabled ? "enabled" : "disabled");
    // Command tracking on  - :X122#
    //         tracking off - :X120#

    char response[AVALON_RESPONSE_BUFFER_LENGTH] = {0};
    if (! sendQuery(enabled ? ":X122#" : ":X120#", response, 0))
    {
        LOGF_ERROR("Failed to %s tracking", enabled ? "enable" : "disable");
        return false;
    }
    return true;
}

bool LX200StarGo::SetTrackRate(double raRate, double deRate)
{
    LOG_DEBUG(__FUNCTION__);
    INDI_UNUSED(deRate);
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];
    int rate = static_cast<int>(raRate);
    sprintf(cmd, ":X1E%04d", rate);
    if(!sendQuery(cmd, response, 0))
    {
        LOGF_ERROR("Failed to set tracking t %d", rate);
        return false;
    }
    return true;
}

void LX200StarGo::ISGetProperties(const char *dev)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) != 0)
        return;

    LX200Telescope::ISGetProperties(dev);
    if (isConnected())
    {
        if (HasTrackMode() && TrackModeS != nullptr)
            defineProperty(&TrackModeSP);
        if (CanControlTrack())
            defineProperty(&TrackStateSP);
        //        if (HasTrackRate())
        //            defineProperty(&TrackRateNP);
    }
    /*
        if (isConnected())
        {
            if (genericCapability & LX200_HAS_ALIGNMENT_TYPE)
                defineProperty(&AlignmentSP);

            if (genericCapability & LX200_HAS_TRACKING_FREQ)
                defineProperty(&TrackingFreqNP);

            if (genericCapability & LX200_HAS_PULSE_GUIDING)
                defineProperty(&UsePulseCmdSP);

            if (genericCapability & LX200_HAS_SITES)
            {
                defineProperty(&SiteSP);
                defineProperty(&SiteNameTP);
            }

            defineProperty(&GuideNSNP);
            defineProperty(&GuideWENP);

            if (genericCapability & LX200_HAS_FOCUS)
            {
                defineProperty(&FocusMotionSP);
                defineProperty(&FocusTimerNP);
                defineProperty(&FocusModeSP);
            }
        }
        */
}

bool LX200StarGo::Goto(double ra, double dec)
{
    LOG_DEBUG(__FUNCTION__);
    const struct timespec timeout = {0, 100000000L};

    targetRA  = ra;
    targetDEC = dec;

    //    fs_sexa(RAStr, targetRA, 2, fracbase);
    //    fs_sexa(DecStr, targetDEC, 2, fracbase);

    // If moving, let's stop it first.
    if (EqNP.s == IPS_BUSY)
    {
        if (!isSimulation() && !Abort())
        {
            AbortSP.s = IPS_ALERT;
            IDSetSwitch(&AbortSP, "Abort slew failed.");
            return false;
        }

        AbortSP.s = IPS_OK;
        EqNP.s    = IPS_IDLE;
        IDSetSwitch(&AbortSP, "Slew aborted.");
        IDSetNumber(&EqNP, nullptr);

        if (MovementNSSP.s == IPS_BUSY || MovementWESP.s == IPS_BUSY)
        {
            MovementNSSP.s = MovementWESP.s = IPS_IDLE;
            EqNP.s                          = IPS_IDLE;
            IUResetSwitch(&MovementNSSP);
            IUResetSwitch(&MovementWESP);
            IDSetSwitch(&MovementNSSP, nullptr);
            IDSetSwitch(&MovementWESP, nullptr);
        }

        // sleep for 100 mseconds
        nanosleep(&timeout, nullptr);
    }
    if(!isSimulation() && !setObjectCoords(ra, dec))
    {
        LOG_ERROR("Error setting coords for goto");
        return false;
    }

    if (!isSimulation())
    {
        char response[AVALON_RESPONSE_BUFFER_LENGTH];
        if(!sendQuery(":MS#", response))
            /* Slew reads the '0', that is not the end of the slew */
            //        if ((err = Slew(PortFD)))
        {
            LOG_ERROR("Error Slewing");
            slewError(0);
            return false;
        }
    }

    TrackState = SCOPE_SLEWING;
    //EqNP.s     = IPS_BUSY;

    //    LOGF_INFO("Slewing to RA: %s - DEC: %s", RAStr, DecStr);

    return true;
}

bool LX200StarGo::MoveNS(INDI_DIR_NS dir, TelescopeMotionCommand command)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];

    sprintf(cmd, ":%s%s#", command == MOTION_START ? "M" : "Q", dir == DIRECTION_NORTH ? "n" : "s");
    if (!isSimulation() && !sendQuery(cmd, response, 0))
    {
        LOG_ERROR("Error N/S motion direction.");
        return false;
    }

    return true;
}

bool LX200StarGo::MoveWE(INDI_DIR_WE dir, TelescopeMotionCommand command)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];

    sprintf(cmd, ":%s%s#", command == MOTION_START ? "M" : "Q", dir == DIRECTION_WEST ? "w" : "e");

    if (!isSimulation() && !sendQuery(cmd, response, 0))
    {
        LOG_ERROR("Error W/E motion direction.");
        return false;
    }

    return true;
}

bool LX200StarGo::Abort()
{
    LOG_DEBUG(__FUNCTION__);
    //   char cmd[AVALON_COMMAND_BUFFER_LENGTH];
    char response[AVALON_RESPONSE_BUFFER_LENGTH];
    if (!isSimulation() && !sendQuery(":Q#", response, 0))
    {
        LOG_ERROR("Failed to abort slew.");
        return false;
    }

    if (GuideNSNP.s == IPS_BUSY || GuideWENP.s == IPS_BUSY)
    {
        GuideNSNP.s = GuideWENP.s = IPS_IDLE;
        GuideNSN[0].value = GuideNSN[1].value = 0.0;
        GuideWEN[0].value = GuideWEN[1].value = 0.0;

        if (GuideNSTID)
        {
            IERmTimer(GuideNSTID);
            GuideNSTID = 0;
        }

        if (GuideWETID)
        {
            IERmTimer(GuideWETID);
            GuideNSTID = 0;
        }

        LOG_INFO("Guide aborted.");
        IDSetNumber(&GuideNSNP, nullptr);
        IDSetNumber(&GuideWENP, nullptr);

        return true;
    }

    return true;
}

bool LX200StarGo::Sync(double ra, double dec)
{
    LOG_DEBUG(__FUNCTION__);
    //   char syncString[256]={0};
    char response[AVALON_RESPONSE_BUFFER_LENGTH];

    if(!isSimulation() && !setObjectCoords(ra, dec))
    {
        LOG_ERROR("Error setting coords for sync");
        return false;
    }

    if (!isSimulation() && !sendQuery(":CM#", response))
    {
        EqNP.s = IPS_ALERT;
        IDSetNumber(&EqNP, "Synchronization failed.");
        return false;
    }

    currentRA  = ra;
    currentDEC = dec;

    LOG_INFO("Synchronization successful.");

    EqNP.s     = IPS_OK;

    NewRaDec(currentRA, currentDEC);

    return true;
}

bool LX200StarGo::setObjectCoords(double ra, double dec)
{
    LOG_DEBUG(__FUNCTION__);

    char RAStr[64] = {0}, DecStr[64] = {0};
    int h, m, s, d;
    getSexComponents(ra, &h, &m, &s);
    snprintf(RAStr, sizeof(RAStr), ":Sr%02d:%02d:%02d#", h, m, s);
    getSexComponents(dec, &d, &m, &s);
    /* case with negative zero */
    if (!d && dec < 0)
        snprintf(DecStr, sizeof(DecStr), ":Sd-%02d*%02d:%02d#", d, m, s);
    else
        snprintf(DecStr, sizeof(DecStr), ":Sd%+03d*%02d:%02d#", d, m, s);
    char response[AVALON_RESPONSE_BUFFER_LENGTH];
    if (isSimulation()) return true;
    // These commands receive a response without a terminating #
    if(!sendQuery(RAStr, response, '1', 2)  || !sendQuery(DecStr, response, '1', 2) )
    {
        EqNP.s = IPS_ALERT;
        IDSetNumber(&EqNP, "Error setting RA/DEC.");
        return false;
    }

    return true;
}

bool LX200StarGo::setLocalDate(uint8_t days, uint8_t months, uint16_t years)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[RB_MAX_LEN] = {0};
    char response[RB_MAX_LEN] = {0};

    int yy = years % 100;

    // Use X50 using DDMMYY
    snprintf(cmd, sizeof(cmd), ":SC %02d%02d%02d#", months, days, yy);
    if (!sendQuery(cmd, response))
        return false;

    if (response[0] == '0')
        return false;

    return true;
}

bool LX200StarGo::setLocalTime24(uint8_t hour, uint8_t minute, uint8_t second)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[RB_MAX_LEN] = {0};
    char response[RB_MAX_LEN] = {0};

    snprintf(cmd, sizeof(cmd), ":SL %02d:%02d:%02d#", hour, minute, second);

    return (sendQuery(cmd, response, 0));
}

bool LX200StarGo::setUTCOffset(double offset)
{
    LOG_DEBUG(__FUNCTION__);
    char cmd[RB_MAX_LEN] = {0};
    char response[RB_MAX_LEN] = {0};
    int hours = static_cast<int>(offset * -1.0);

    snprintf(cmd, sizeof(cmd), ":SG %+03d#", hours);

    return (sendQuery(cmd, response, 0));
}

bool LX200StarGo::getLocalTime(char *timeString)
{
    LOG_DEBUG(__FUNCTION__);
    if (isSimulation())
    {
        time_t now = time (nullptr);
        strftime(timeString, 32, "%T", localtime(&now));
    }
    else
    {
        double ctime = 0;
        int h, m, s;
        char response[RB_MAX_LEN] = {0};
        // FIXME GL# command does not wrk on StarGo
        if (!sendQuery(":GL#", response))
            return false;

        if (f_scansexa(response, &ctime))
        {
            LOGF_DEBUG("Unable to parse local time response %s", response);
            return false;
        }

        getSexComponents(ctime, &h, &m, &s);
        snprintf(timeString, 32, "%02d:%02d:%02d", h, m, s);
    }

    return true;
}

bool LX200StarGo::getLocalDate(char *dateString)
{
    LOG_DEBUG(__FUNCTION__);
    if (isSimulation())
    {
        time_t now = time (nullptr);
        strftime(dateString, 32, "%F", localtime(&now));
    }
    else
    {
        char response[RB_MAX_LEN] = {0};
        int dd, mm, yy;
        char mell_prefix[3] = {0};
        int vars_read = 0;
        //FIXME GC does not work on StarGo
        if (!sendQuery(":GC#", response))
            return false;
        // StarGo format is MM/DD/YY
        vars_read = sscanf(response, "%d%*c%d%*c%d", &mm, &dd, &yy);
        if (vars_read < 3)
        {
            LOGF_ERROR("Cant read date from mount %s", response);
            return false;
        }
        /* We consider years 50 or more to be in the last century, anything less in the 21st century.*/
        if (yy > 50)
            strncpy(mell_prefix, "19", 3);
        else
            strncpy(mell_prefix, "20", 3);
        /* We need to have it in YYYY-MM-DD ISO format */
        snprintf(dateString, 32, "%s%02d-%02d-%02d", mell_prefix, yy, mm, dd);
    }
    return true;
}

bool LX200StarGo::getUTFOffset(double *offset)
{
    LOG_DEBUG(__FUNCTION__);
    if (isSimulation())
    {
        *offset = 3;
        return true;
    }

    int lx200_utc_offset = 0;
    char response[RB_MAX_LEN] = {0};
    float temp_number;

    if (!sendQuery(":GG#", response))
        return false;

    /* Float */
    if (strchr(response, '.'))
    {
        if (sscanf(response, "%f", &temp_number) != 1)
            return false;
        lx200_utc_offset = static_cast<int>(temp_number);
    }
    /* Int */
    else if (sscanf(response, "%d", &lx200_utc_offset) != 1)
        return false;

    // LX200 TimeT Offset is defined at the number of hours added to LOCAL TIME to get TimeT. This is contrary to the normal definition.
    *offset = lx200_utc_offset * -1;
    return true;
}

bool LX200StarGo::getTrackFrequency(double *value)
{
    LOG_DEBUG(__FUNCTION__);
    float Freq;
    char response[RB_MAX_LEN] = {0};

    if (!sendQuery(":GT#", response))
        return false;

    if (sscanf(response, "%f#", &Freq) < 1)
    {
        LOG_ERROR("Unable to parse response");
        return false;
    }

    *value = static_cast<double>(Freq);
    return true;
}

