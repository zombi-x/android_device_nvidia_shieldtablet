/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2012-2014, NVIDIA CORPORATION.  All rights reserved.
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
#define LOG_TAG "powerHAL::common"

#include <hardware/hardware.h>
#include <hardware/power.h>

#include "powerhal_utils.h"
#include "powerhal.h"

#ifdef POWER_MODE_SET_INTERACTIVE
static int get_system_power_mode(void);
static void set_interactive_governor(int mode);

static const interactive_data_t interactive_data_array[NVCPL_HINT_COUNT+1] =
{
    { "1122000", "65 304000:75 1122000:80", "19000", "20000", "0", "41000", "90" },
    { "1020000", "65 256000:75 1020000:80", "19000", "20000", "0", "30000", "99" },
    { "640000", "65 256000:75 640000:80", "80000", "20000", "2", "30000", "99" },
    { "1020000", "65 256000:75 1020000:80", "19000", "20000", "0", "30000", "99" },
    { "420000", "80",                     "80000", "300000","2", "30000", "99" }
};
#endif

static const int VSyncActiveBoostFreq = 300000;

static int get_input_count(void)
{
    int i = 0;
    int ret;
    char path[80];
    char name[32];

    while(1)
    {
        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        ret = access(path, F_OK);
        if (ret < 0)
            break;
        memset(name, 0, 32);
        sysfs_read(path, name, 32);
        ALOGI("input device id:%d present with name:%s", i++, name);
    }
    return i;
}

static void find_input_device_ids(struct powerhal_info *pInfo)
{
    int i = 0;
    int status;
    int count = 0;
    char path[80];
    char name[MAX_CHARS];

    while (1)
    {
        snprintf(path, sizeof(path), "/sys/class/input/input%d/name", i);
        if (access(path, F_OK) < 0)
            break;
        else {
            memset(name, 0, MAX_CHARS);
            sysfs_read(path, name, MAX_CHARS);
            for (int j = 0; j < pInfo->input_cnt; j++) {
                status = (-1 == pInfo->input_devs[j].dev_id)
                    && (0 == strncmp(name,
                    pInfo->input_devs[j].dev_name, MAX_CHARS));
                if (status) {
                    ++count;
                    pInfo->input_devs[j].dev_id = i;
                    ALOGI("find_input_device_ids: %d %s",
                        pInfo->input_devs[j].dev_id,
                        pInfo->input_devs[j].dev_name);
                }
            }
            ++i;
        }

        if (count == pInfo->input_cnt)
            break;
    }
}

static int check_hint(struct powerhal_info *pInfo, power_hint_t hint, uint64_t *t)
{
    struct timespec ts;
    uint64_t time;

    if (hint >= MAX_POWER_HINT_COUNT) {
        ALOGE("Invalid power hint: 0x%x", hint);
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    time = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    if (pInfo->hint_time[hint] && pInfo->hint_interval[hint] &&
        (time - pInfo->hint_time[hint] < pInfo->hint_interval[hint]))
        return -1;

    *t = time;

    return 0;
}

static bool is_available_frequency(struct powerhal_info *pInfo, int freq)
{
    int i;

    for(i = 0; i < pInfo->num_available_frequencies; i++) {
        if(pInfo->available_frequencies[i] == freq)
            return true;
    }

    return false;
}

void common_power_open(struct powerhal_info *pInfo)
{
    int i;
    int size = 256;
    char *pch;

    if (0 == pInfo->input_devs || 0 == pInfo->input_cnt)
        pInfo->input_cnt = get_input_count();
    else
        find_input_device_ids(pInfo);

    // Initialize timeout poker
    Barrier readyToRun;
    pInfo->mTimeoutPoker = new TimeoutPoker(&readyToRun);
    readyToRun.wait();

    // Read available frequencies
    char *buf = (char*)malloc(sizeof(char) * size);
    memset(buf, 0, size);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);

    // Determine number of available frequencies
    pch = strtok(buf, " ");
    pInfo->num_available_frequencies = -1;
    while(pch != NULL)
    {
        pch = strtok(NULL, " ");
        pInfo->num_available_frequencies++;
    }

    // Store available frequencies in a lookup array
    pInfo->available_frequencies = (int*)malloc(sizeof(int) * pInfo->num_available_frequencies);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);
    pch = strtok(buf, " ");
    for(i = 0; i < pInfo->num_available_frequencies; i++)
    {
        pInfo->available_frequencies[i] = atoi(pch);
        pch = strtok(NULL, " ");
    }

    // Store LP cluster max frequency
    sysfs_read("/sys/devices/system/cpu/cpuquiet/tegra_cpuquiet/idle_top_freq",
                buf, size);
    pInfo->lp_max_frequency = atoi(buf);

    pInfo->interaction_boost_frequency = pInfo->lp_max_frequency;
    pInfo->animation_boost_frequency = pInfo->lp_max_frequency;

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1326000) {
            pInfo->interaction_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    }

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1044000) {
            pInfo->animation_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    } 

    // Store CPU0 max frequency
    sysfs_read(SYS_NODE_CPU0_MAX_FREQ, buf, size);
    pInfo->cpu0_max_frequency = atoi(buf);

    // Initialize hint intervals in usec
    //
    // Set the interaction timeout to be slightly shorter than the duration of
    // the interaction boost so that we can maintain is constantly during
    // interaction.
    pInfo->hint_interval[POWER_HINT_INTERACTION] = 90000;
    pInfo->hint_interval[POWER_HINT_CPU_BOOST] = 1000000;
    pInfo->hint_interval[POWER_HINT_LAUNCH_BOOST] = 1000000;
    //pInfo->hint_interval[POWER_HINT_SHIELD_STREAMING] = 500000;
    //pInfo->hint_interval[POWER_HINT_HIGH_RES_VIDEO] = 500000;
    //pInfo->hint_interval[POWER_HINT_MIRACAST] = 500000;
    pInfo->hint_interval[POWER_HINT_AUDIO] = 700000;
    //pInfo->hint_interval[POWER_HINT_AUDIO_OTHER] = 700000;
    //pInfo->hint_interval[POWER_HINT_AUDIO_SPEAKER] = 700000;
    //pInfo->hint_interval[POWER_HINT_DISPLAY_ROTATION] = 200000;
    pInfo->hint_interval[POWER_HINT_LOW_POWER] = 0;

    pInfo->fds.vsync_min_cpu = -1;

    // Initialize features
    // pInfo->features.fan = sysfs_exists("/sys/devices/platform/pwm-fan/pwm_cap");

    free(buf);
}

static void set_vsync_min_cpu_freq(struct powerhal_info *pInfo, int enabled)
{
    if (enabled && pInfo->fds.vsync_min_cpu == -1) {
        pInfo->fds.vsync_min_cpu =
        pInfo->mTimeoutPoker->requestPmQos(PMQOS_CONSTRAINT_CPU_FREQ, PM_QOS_BOOST_PRIORITY, PM_QOS_DEFAULT_VALUE, VSyncActiveBoostFreq);
    } else if (!enabled && pInfo->fds.vsync_min_cpu >= 0) {
        close(pInfo->fds.vsync_min_cpu);
        pInfo->fds.vsync_min_cpu = -1;
    }

    ALOGV("%s: set min CPU floor =%i", __func__, VSyncActiveBoostFreq);
}

static void set_prism_control_enable(struct powerhal_info *pInfo, int value)
{
    if (value)
        set_property_int(PRISM_CONTROL_PROP, 1);
    else
        set_property_int(PRISM_CONTROL_PROP, 0);
    ALOGV("%s: set prism enable =%d", __func__, value);
}

static void set_pbc_power(struct powerhal_info *pInfo, int value)
{
    if (value < 0)
        value = pInfo->defaults.power_cap;

    set_property_int(POWER_CAP_PROP, value);
}

void common_power_init(struct power_module __unused *module, struct powerhal_info *pInfo)
{
    common_power_open(pInfo);

    pInfo->ftrace_enable = get_property_bool("nvidia.hwc.ftrace_enable", false);

    // Boost to max frequency on initialization to decrease boot time
    pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                            PM_QOS_BOOST_PRIORITY,
                                            PM_QOS_DEFAULT_VALUE,
                                            pInfo->available_frequencies[pInfo->num_available_frequencies - 1],
                                            s2ns(15));
}

void common_power_set_interactive(struct power_module __unused *module, struct powerhal_info *pInfo, int on)
{
    int i;
    int dev_id;
    char path[80];
    const char* state = (0 == on)?"0":"1";
    int power_mode = -1;

    sysfs_write("/sys/devices/platform/host1x/nvavp/boost_sclk", state);

    if (0 != pInfo) {
        for (i = 0; i < pInfo->input_cnt; i++) {
            if (0 == pInfo->input_devs)
                dev_id = i;
            else if (-1 == pInfo->input_devs[i].dev_id)
                continue;
            else
                dev_id = pInfo->input_devs[i].dev_id;
            snprintf(path, sizeof(path), "/sys/class/input/input%d/enabled", dev_id);
            if (!access(path, W_OK)) {
                if (0 == on)
                    ALOGI("Disabling input device:%d", dev_id);
                else
                    ALOGI("Enabling input device:%d", dev_id);
                sysfs_write(path, state);
            }
        }
    }

#ifdef POWER_MODE_SET_INTERACTIVE
    if (on) {
        power_mode = get_system_power_mode();
        if (power_mode < NVCPL_HINT_MAX_PERF || power_mode > NVCPL_HINT_COUNT) {
            ALOGV("%s: no system power mode info, take optimized settings", __func__);
            power_mode = NVCPL_HINT_OPT_PERF;
        }
    } else {
        power_mode = NVCPL_HINT_COUNT;
    }
    set_interactive_governor(power_mode);
#else
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq", (on == 0)?"420000":"624000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/target_loads", (on == 0)?"45 312000:75 564000:85":"65 228000:75 624000:85");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay", (on == 0)?"80000":"19000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate", (on == 0)?"300000":"20000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/boost_factor", (on == 0)?"2":"0");
#endif
}

#ifdef POWER_MODE_SET_INTERACTIVE
static int get_system_power_mode(void)
{
    char value[PROPERTY_VALUE_MAX] = { 0 };
    int power_mode = -1;

    property_get("persist.sys.NV_POWER_MODE", value, "");
    if (value[0] != '\0')
    {
        power_mode = atoi(value);
    }

    if (get_property_bool("persist.sys.NV_ECO.STATE.ISECO", false))
    {
        power_mode = NVCPL_HINT_BAT_SAVE;
    }

    return power_mode;
}

static void __sysfs_write(const char *file, const char *data)
{
    if (data != NULL)
    {
        sysfs_write(file, data);
    }
}

static void set_interactive_governor(int mode)
{
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq",
            interactive_data_array[mode].hispeed_freq);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/target_loads",
            interactive_data_array[mode].target_loads);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay",
            interactive_data_array[mode].above_hispeed_delay);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate",
            interactive_data_array[mode].timer_rate);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/boost_factor",
            interactive_data_array[mode].boost_factor);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time",
            interactive_data_array[mode].min_sample_time);
    __sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/go_hispeed_load",
            interactive_data_array[mode].go_hispeed_load);
}

static void set_power_mode_hint(struct powerhal_info *pInfo, nvcpl_hint_t *data)
{
    int mode = data[0];
    int status;
    char value[4] = { 0 };

    if (mode < NVCPL_HINT_MAX_PERF || mode > NVCPL_HINT_COUNT)
    {
        ALOGE("%s: invalid hint mode = %d", __func__, mode);
        return;
    }

    // only set interactive governor parameters when display on
    sysfs_read("/sys/class/backlight/pwm-backlight/brightness", value, sizeof(value));
    status = atoi(value);

    if (status)
    {
        set_interactive_governor(mode);
    }

}
#endif

void common_power_hint(struct power_module __unused *module, struct powerhal_info *pInfo,
                            power_hint_t hint, void *data)
{
    uint64_t t;

    if (!pInfo)
        return;

    if (check_hint(pInfo, hint, &t) < 0)
        return;

    switch (hint) {
    case POWER_HINT_VSYNC:
        if (data)
            set_vsync_min_cpu_freq(pInfo, *(int *)data);
        break;
    case POWER_HINT_INTERACTION:
        if (pInfo->ftrace_enable) {
            sysfs_write("/sys/kernel/debug/tracing/trace_marker", "Start POWER_HINT_INTERACTION\n");
        }
        // Boost to interaction_boost_frequency
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_ONLINE_CPUS,
                                                PM_QOS_BOOST_PRIORITY,
                                                4,
                                                2,
                                                ms2ns(500));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                1530000,
                                                1044000,
                                                ms2ns(500));
        // During the animation, we need some level of CPU/GPU/EMC frequency floor
        // to get perfect animation. Forcing CPU frequency through PMQoS does not
        // scale EMC fast enough so EMC frequency boosting should be placed first.
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_GPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                852000,
                                                72000,
                                                ms2ns(500));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                 396000,
                                                 s2ns(2));
        break;
    case POWER_HINT_LAUNCH_BOOST:
        // Boost to 4 cores
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_ONLINE_CPUS,
                                                PM_QOS_BOOST_PRIORITY,
                                                4,
                                                4,
                                                s2ns(2));
        break;
    case POWER_HINT_CPU_BOOST:
        // Boost to 1.2Ghz dual core
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                1224000,
                                                ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_ONLINE_CPUS,
                                                PM_QOS_BOOST_PRIORITY,
                                                4,
                                                2,
                                                ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_GPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                852000,
                                                180000,
                                                ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                792000,
                                                ms2ns(1500));
        break;
/*    case POWER_HINT_SHIELD_STREAMING:
        // Boost to 816 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                828000,
                                                s2ns(1));
        break;
    case POWER_HINT_HIGH_RES_VIDEO:
        // Boost to max LP frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                pInfo->lp_max_frequency,
                                                s2ns(1));
        break;
    case POWER_HINT_MIRACAST:
        // Boost to 816 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                828000,
                                                s2ns(1));
        break;
    case POWER_HINT_DISPLAY_ROTATION:
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                1530000,
                                                s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_ONLINE_CPUS,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                2,
                                                s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_GPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                252000,
                                                s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                 396000,
                                                 s2ns(2));
        break;
*/
    case POWER_HINT_AUDIO:
        // Boost to 512 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                564000,
                                                s2ns(1));
        break;
/*    case POWER_HINT_AUDIO_OTHER:
        // Boost to 512 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                564000,
                                                s2ns(1));
        break;
    case POWER_HINT_AUDIO_SPEAKER:
        // Boost to 512 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_CONSTRAINT_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                PM_QOS_DEFAULT_VALUE,
                                                564000,
                                                s2ns(1));
        break;
*/
    case POWER_HINT_LOW_POWER:
        // Drop max frequencies and limit to one core for low power mode
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_MAX_CPU_FREQ,
                                                PM_QOS_BOOST_PRIORITY,
                                                1044000,
                                                564000,
                                                s2ns(1));
        pInfo->mTimeoutPoker->requestPmQosTimed(PMQOS_MAX_ONLINE_CPUS,
                                                PM_QOS_BOOST_PRIORITY,
                                                2,
                                                1,
                                                s2ns(1));
        break;
    default:
        ALOGE("Unknown power hint: 0x%x", hint);
        break;
    }

    pInfo->hint_time[hint] = t;
}
