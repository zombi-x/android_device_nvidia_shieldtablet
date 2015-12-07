/* Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2011-2014, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>

#include "nvs_input.h"
#include "lightsensor.h"
#include "max44005.h"
#include "ltr558als.h"
#include "MPLSensor.h"
#include "MPLSensorDefs.h"
#include "CompassSensor.h"
#include "bmpx80.h"

/*
 * 10 is 3 bigger than the size it looks to be here in order to accomodate
 * stm8t143 proximity sensor, Pressure and ALS sensor (Cm3217 or Cm3218
 * depending on if it is TN8 or shield_ers respectively)
 *
 * NOTE:
 * For each sensor defn in sSensorList, a corresponding enum in sensor_id
 * has to be present. Also, the mapping from the corresponding handle to
 * sensor_id should be defined in handleToSensor. As a result, the order
 * in which the following sensor defn are enrolled does not matter.
 */
static struct sensor_t sSensorList[10] = {
      MPLROTATIONVECTOR_DEF,
      MPLLINEARACCEL_DEF,
      MPLGRAVITY_DEF,
      MPLGYRO_DEF,
      MPLACCEL_DEF,
      MPLMAGNETICFIELD_DEF,
      MPLORIENTATION_DEF,
};

/*****************************************************************************/

static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device);


static int sensors__get_sensors_list(struct sensors_module_t* module,
                                     struct sensor_t const** list)
{
    *list = sSensorList;
    int size = ARRAY_SIZE(sSensorList) - 3;

    Bmpx80Pressure::fillSensorDef(sSensorList, size);
    /*
     * NOTE: LightSensorBase and Max44005Base do not detect same sensors.
     * Hence, it is ok to have them called independently.
     */
    AmbientLightSensor::fillSensorDef(sSensorList, size);
    Max44005Light::fillSensorDef(sSensorList, size);

    /* LTR659 proximity sensor. */
    ltr558Prox::fillSensorDef(sSensorList, size);

    return size;
}

static struct hw_module_methods_t sensors_module_methods = {
        open: open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
        common: {
                tag: HARDWARE_MODULE_TAG,
                version_major: 1,
                version_minor: 0,
                id: SENSORS_HARDWARE_MODULE_ID,
                name: "TN8 sensors module",
                author: "nvidia",
                methods: &sensors_module_methods,
                dso: NULL,
                reserved: {0}
        },
        get_sensors_list: sensors__get_sensors_list,
};

struct sensors_poll_context_t {
    struct sensors_poll_device_t device; // must be first

    sensors_poll_context_t();
    ~sensors_poll_context_t();
    int activate(int handle, int enabled);
    int setDelay(int handle, int64_t ns);
    int pollEvents(sensors_event_t* data, int count);
    unsigned int polltime;

private:
    enum {
        light             = 0,
        mpl               = 1,
        pressure,
        compass,
        proximity,
        numSensorDrivers,       // wake pipe goes here
        numFds,
    };

    /* Should be in sync with struct sensor_t sSensorList[] */
    enum sensor_ids {
        SENSOR_RV = 0,
        SENSOR_LA,
        SENSOR_GR,
        SENSOR_GY,
        SENSOR_A,
        SENSOR_M,
        SENSOR_O,
        SENSOR_L,
        SENSOR_AP,
        SENSOR_P,
        MAX_SENSOR_TYPES,
    };

    static const size_t wake = numFds - 1;
    static const char WAKE_MESSAGE = 'W';
    struct pollfd mPollFds[numFds];
    SensorBase* mSensors[numSensorDrivers];
    bool isSensorEnabled[MAX_SENSOR_TYPES];
    unsigned int requestedPollTime[MAX_SENSOR_TYPES];
    int mWritePipeFd;

    int handleToDriver(int handle) const {
        switch (handle) {
            case ID_RV:
            case ID_LA:
            case ID_GR:
            case ID_GY:
            case ID_A:
            case ID_O:
            case ID_M:
                return mpl;

            case ID_L:
                return light;

            case ID_AP:
                return pressure;

            case ID_P:
                return proximity;
        }

        return -EINVAL;
    }

    int inputDevPathNum(const char *dev_name) {
        char name[32] = {0};
        char path[80];
        int i;
        int fd;
        int err;

        i = 0;
        while(1)
        {
            sprintf(path, "/sys/class/input/input%d/name", i);
            err = access(path, F_OK);
            if (err < 0) {
                break;
            } else {
                fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    memset(name, 0, sizeof(name));
                    read(fd, name, sizeof(name));
                    close(fd);
                    if (!strncmp(name, dev_name, strlen(dev_name))) {
                        ALOGI("%s %s %s found", __func__, path, dev_name);
                        err = i;
                        break;
                    }
                }
            }
            i++;
        }

        return err;
    }

    int handleToSensor(int handle) {
        switch (handle) {
            case ID_RV: return SENSOR_RV;
            case ID_LA: return SENSOR_LA;
            case ID_GR: return SENSOR_GR;
            case ID_GY: return SENSOR_GY;
            case ID_A : return SENSOR_A;
            case ID_O : return SENSOR_O;
            case ID_M : return SENSOR_M;
            case ID_L : return SENSOR_L;
            case ID_P : return SENSOR_P;
        }

        return -EINVAL;
    }

    void setPollTime() {
        unsigned int poll_time = UINT_MAX;
        for (unsigned int i = 0; i < ARRAY_SIZE(sSensorList); i++) {
            unsigned int poll_time_tmp = requestedPollTime[i];
            if (poll_time_tmp < poll_time)
                poll_time = poll_time_tmp;
        }
        polltime = poll_time;
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result < 0, "error sending wake message (%s)", strerror(errno));
    }

    /* poll should wait indefinitely for interrupt based sensors
     * and poll should not block others, hence timeout is required
     */
    void updateSensorActivate(int handle, int activate) {
        int driver = handleToDriver(handle);
        if (mPollFds[driver].fd != -1)
            return;
        int sensor_type = handleToSensor(handle);
        if (sensor_type < 0)
            return;
        isSensorEnabled[sensor_type] = activate;
        if (!activate)
            requestedPollTime[sensor_type] = UINT_MAX;
        setPollTime();
    }

    void updateSensorPollTime(int handle, unsigned int ms) {
        int driver = handleToDriver(handle);
        if (mPollFds[driver].fd != -1)
            return;
        int sensor_type = handleToSensor(handle);
        if (sensor_type < 0)
            return;
        if ((isSensorEnabled[sensor_type]) && (requestedPollTime[sensor_type] > ms)) {
            requestedPollTime[sensor_type] = ms;
        }
        setPollTime();
    }
};

/*****************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
    VFUNC_LOG;

    int inputNum;
    unsigned i;
    int fd;
    int flags;
    char name[32] = {0};
    char path[80];

    polltime = UINT_MAX;

    ALOGE("sensors_poll_context_t started");

    memset(&mPollFds, 0, sizeof(mPollFds));
    for (i = 0; i < numSensorDrivers; i++) {
        mSensors[i] = NULL;
    }

    for (i = 0; i < MAX_SENSOR_TYPES; i++) {
        requestedPollTime[i] = UINT_MAX;
        isSensorEnabled[i] = 0;
    }

    CompassSensor *mCompassSensor = NULL;
    inputNum = inputDevPathNum("akm89xx");
    if (inputNum >= 0)
        mCompassSensor = new CompassSensor("akm89xx", inputNum, 0);
    MPLSensor *mplSensor = new MPLSensor(mCompassSensor);

    // setup the callback object for handing mpl callbacks
    setCallbackObject(mplSensor);
    mSensors[mpl] = mplSensor;
    mPollFds[mpl].fd = mSensors[mpl]->getFd();
    flags = fcntl(mPollFds[mpl].fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(mPollFds[mpl].fd, F_SETFL, flags);
    mPollFds[mpl].events = POLLIN;
    mPollFds[mpl].revents = 0;

    if (mCompassSensor != NULL) {
        mSensors[compass] = mCompassSensor;
        mPollFds[compass].fd = mplSensor->getCompassFd();
        flags = fcntl(mPollFds[compass].fd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(mPollFds[compass].fd, F_SETFL, flags);
        mPollFds[compass].events = POLLIN;
        mPollFds[compass].revents = 0;
    }

    /* Cm3217 ALS on TN8 or Cm3218 ALS on shield_ers */
    mSensors[light] = LightSensorBase::getInstance(ID_L);
    if (!mSensors[light])
        mSensors[light] = Max44005Light::getInstance();
    mPollFds[light].fd = -1;

    char prox_path[MAX_SENSOR_PATH_LEN] = {0};
    char prox_enable_path[MAX_SENSOR_PATH_LEN] = {0};
    char prox_thres_path[MAX_SENSOR_PATH_LEN] = {0};
    if (ltr558Prox::fillPaths(prox_path, prox_enable_path, prox_thres_path))
        mSensors[proximity] = new ltr558Prox(prox_path, prox_enable_path,
                                             prox_thres_path, ID_P);

    mPollFds[proximity].fd = -1;

    inputNum = inputDevPathNum(BMP180_DEV_NAME);
    if (inputNum >= 0) {
        mSensors[pressure] = new NvsInput(BMP180_DEV_NAME, inputNum,
                                          ID_AP, SENSOR_TYPE_PRESSURE, 0);
        mPollFds[pressure].fd = mSensors[pressure]->getFd();
        flags = fcntl(mPollFds[pressure].fd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl(mPollFds[pressure].fd, F_SETFL, flags);
        mPollFds[pressure].events = POLLIN;
        mPollFds[pressure].revents = 0;
    }

    int wakeFds[2];
    int result = pipe(wakeFds);
    ALOGE_IF(result < 0, "error creating wake pipe (%s)", strerror(errno));
    fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
    mWritePipeFd = wakeFds[1];

    mPollFds[wake].fd = wakeFds[0];
    mPollFds[wake].events = POLLIN;
    mPollFds[wake].revents = 0;
}

sensors_poll_context_t::~sensors_poll_context_t()
{
    VFUNC_LOG;

    unsigned i;

    for (i = 0; i < numSensorDrivers; i++) {
        if (mSensors[i] != NULL)
            delete mSensors[i];
    }
    close(mPollFds[wake].fd);
    close(mWritePipeFd);
}

int sensors_poll_context_t::activate(int handle, int enabled)
{
    VFUNC_LOG;

    int err;
    int index = handleToDriver(handle);

    if (index < 0)
        return index;

    if (mSensors[index] == NULL)
        return 0;

    err =  mSensors[index]->enable(handle, enabled);
    if (!err) {
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result < 0, "error sending wake message (%s)", strerror(errno));
        updateSensorActivate(handle, enabled);
    } else {
        ALOGE("enable sensor error! handle: %d", handle);
    }
    return err;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns)
{
    VFUNC_LOG;

    int index = handleToDriver(handle);

    if (index < 0)
        return index;

    if (mSensors[index] == NULL)
        return 0;

    updateSensorPollTime(handle, ns/1000000);

    return mSensors[index]->setDelay(handle, ns);
}

int sensors_poll_context_t::pollEvents(sensors_event_t* data, int count)
{
    VHANDLER_LOG;

    unsigned i;
    int nb;
    int nbEvents = 0;
    int n = 0;

    do {
        for (i = 0; i < numSensorDrivers; i++) {
            if (mSensors[i] == NULL)
                continue;

            SensorBase* const sensor(mSensors[i]);
            if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
                if (i == compass)
                    nb = ((MPLSensor*)mSensors[mpl])->readCompassEvents(data, count);
                else
                    nb = sensor->readEvents(data, count);
                if (nb > 0) {
                    count -= nb;
                    nbEvents += nb;
                    data += nb;
                }
                if (nb < count)
                    // no more data for this sensor
                    mPollFds[i].revents = 0;
            }
        }

        if (count) {
            // we still have some room, so try to see if we can get
            // some events immediately or just wait for polltime
            // if we don't have anything to return
            n = poll(mPollFds, numFds, nbEvents ? 0 : polltime);
            if (n < 0) {
                ALOGE("poll() failed (%s)", strerror(errno));
                if (errno == EINTR)
                    return nbEvents;
                else
                    return -errno;
            }

            if (mPollFds[wake].revents & POLLIN) {
                char msg;
                int result = read(mPollFds[wake].fd, &msg, 1);
                ALOGE_IF(result < 0, "error reading from wake pipe (%s)", strerror(errno));
                ALOGE_IF(msg != WAKE_MESSAGE, "unknown message on wake queue (0x%02x)", int(msg));
                mPollFds[wake].revents = 0;
            }
        }
        // if we have events and space, go read them
    } while ((n || !nbEvents) && count);

    return nbEvents;
}

/*****************************************************************************/

static int poll__close(struct hw_device_t *dev)
{
    VFUNC_LOG;

    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;

    if (ctx)
        delete ctx;
    return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev,
                          int handle, int enabled)
{
    VFUNC_LOG;

    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;

    return ctx->activate(handle, enabled);
}

static int poll__setDelay(struct sensors_poll_device_t *dev,
                          int handle, int64_t ns)
{
    VFUNC_LOG;

    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;

    return ctx->setDelay(handle, ns);
}

static int poll__poll(struct sensors_poll_device_t *dev,
                      sensors_event_t* data, int count)
{
    VFUNC_LOG;

    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;

    return ctx->pollEvents(data, count);
}

/*****************************************************************************/

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device)
{
    VFUNC_LOG;

    int status = -EINVAL;

    sensors_poll_context_t *dev = new sensors_poll_context_t();

    memset(&dev->device, 0, sizeof(sensors_poll_device_t));
    dev->device.common.tag = HARDWARE_DEVICE_TAG;
    dev->device.common.version  = 0;
    dev->device.common.module   = const_cast<hw_module_t*>(module);
    dev->device.common.close    = poll__close;
    dev->device.activate        = poll__activate;
    dev->device.setDelay        = poll__setDelay;
    dev->device.poll            = poll__poll;

    *device = &dev->device.common;
    status = 0;
    return status;
}
