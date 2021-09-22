#include "governor.h"
#include "clocks.h"

#define NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD 0x80044715
#define MAXPERCENT  101
#define MAX(a,b)    (a) > (b) ? (a) : (b)
#define MIN(a,b)    (a) > (b) ? (b) : (a)

const uint32_t samplingrate = 60;
const uint32_t ticktime = 1'000'000'000 / samplingrate;
const uint64_t systemtickfreq = 19200000 / samplingrate;

const uint32_t CPUHand[8][2] = 
{
    {612000000,     20},
    {714000000,     50},
    {816000000,     60},
    {918000000,     70},
    {1020000000,    80},
    {1122000000,    90},
    {1224000000,    95},
    {1785000000,    MAXPERCENT},
};

const uint32_t CPUDock[9][2] = 
{
    {612000000,     20},
    {714000000,     40},
    {816000000,     50},
    {918000000,     60},
    {1020000000,    70},
    {1122000000,    80},
    {1224000000,    90},
    {1785000000,    95},
    {2295000000,    MAXPERCENT},
};

const uint32_t GPU230_4[4][2] = 
{
    {76800000,  30},
    {153600000, 50},
    {230400000, 85},
    {307200000, MAXPERCENT},
};

const uint32_t GPU307_2[5][2] = 
{
    {76800000,  25},
    {153600000, 35},
    {230400000, 55},
    {307200000, 85},
    {384000000, MAXPERCENT},
};

const uint32_t GPU384[6][2] = 
{
    {76800000,  20},
    {153600000, 40},
    {230400000, 50},
    {307200000, 70},
    {384000000, 85},
    {460800000, MAXPERCENT},
};

const uint32_t GPU460_8[7][2] = 
{
    {76800000,  20},
    {153600000, 30},
    {230400000, 40},
    {307200000, 50},
    {384000000, 70},
    {460800000, 85},
    {537600000, MAXPERCENT},
};

const uint32_t GPU768[11][2] =
{
    {76800000,   5},
    {153600000, 10},
    {230400000, 20},
    {307200000, 30},
    {384000000, 40},
    {537600000, 50},
    {691200000, 60},
    {768000000, 70},
    {921600000, 80},
    {1075200000,90},
    {1267200000,MAXPERCENT},
};

Thread Core0, Core1, Core2, Core3, GPU, Misc;
bool checkexit = false;

uint64_t idletick_a[4] = {0}, idletick_b[4] = {0}, idletick[4] = {0};
float CPUCoreUsage[4] = { 0. };

Result nvCheck = 1;
uint32_t fd = 0;
uint32_t GPULoad = 0;
float GPULoad_f = 0, GPULoad_fPrev = 0, GPULoadAdjusted = 0;

uint8_t CPUTick = 0;

uint8_t SecondTick = samplingrate;

const uint32_t* CPUTable;
const uint32_t* GPUTable;

SysClkProfile CurProfile;
uint32_t CurConfId;

void CheckCore(uint8_t CoreID)
{
    while(checkexit == false)
    {
        svcGetInfo(&idletick_a[CoreID], InfoType_IdleTickCount, INVALID_HANDLE, CoreID);
        svcSleepThread(ticktime);
        svcGetInfo(&idletick_b[CoreID], InfoType_IdleTickCount, INVALID_HANDLE, CoreID);
        idletick[CoreID] = idletick_b[CoreID] - idletick_a[CoreID];

        if (idletick[CoreID] > systemtickfreq)
            CPUCoreUsage[CoreID] = 0.;
        else
            CPUCoreUsage[CoreID] = ((double)systemtickfreq - (double)idletick[CoreID]) / (double)systemtickfreq * 100;

        CPUTick++;
    }
}

void CheckCore0(void*)
{
    uint8_t CoreID = 0;
    CheckCore(CoreID);
}

void CheckCore1(void*)
{
    uint8_t CoreID = 1;
    CheckCore(CoreID);
}

void CheckCore2(void*)
{
    uint8_t CoreID = 2;
    CheckCore(CoreID);
}

void CheckCore3(void*)
{
    uint8_t CoreID = 3;
    CheckCore(CoreID);
}

void SetTable(uint32_t ConfId, SysClkProfile ConfProfile)
{
    //CPU Table
    if(ConfProfile == SysClkProfile_Docked || ConfProfile == SysClkProfile_HandheldChargingOfficial)
        CPUTable = &CPUDock[0][0];
    else
        CPUTable = &CPUHand[0][0];
    //GPU Table
    switch(ConfId)
    {
        case 00020000:
        case 0x00020002:
            GPUTable = &GPU230_4[0][0];
            break;
        case 0x00020001:
        case 0x00020003:
        case 0x00020005:
            GPUTable = &GPU307_2[0][0];
            break;
        case 0x00010000:
        case 0x00020004:
        case 0x00020006:
            GPUTable = &GPU384[0][0];
            break;
        case 0x92220007:
        case 0x92220008:
            GPUTable = &GPU460_8[0][0];
            break;
        case 0x00010001:
        case 0x00010002:
            GPUTable = &GPU768[0][0];
            break;
        default:
            GPUTable = &GPU384[0][0];
    }
}

void CheckGPU_Set(void*)
{
    Result rc;
    rc = nvOpen(&fd, "/dev/nvhost-ctrl-gpu");
    if (R_FAILED(rc))
        FileUtils::LogLine("[gov] nvOpen(...): %lx", rc);
    while(checkexit == false)
    {
        rc = nvIoctl(fd, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &GPULoad);
        if (R_FAILED(rc))
            FileUtils::LogLine("[gov] nvIoctl(...): %lx", rc);

        GPULoad_fPrev = GPULoad_f;
        GPULoad_f = (float)GPULoad / 10;
        GPULoadAdjusted = 0.4*GPULoad_fPrev+0.6*GPULoad_f;

        for(short i = 0; i < 11*2; i += 2)
        {
            if(GPULoadAdjusted < *(GPUTable + i + 1))
            {
                static uint32_t GPUClk;
                GPUClk = *(GPUTable + i);
                Clocks::SetHz(SysClkModule_GPU, GPUClk);
                break;
            }
        }

        SecondTick++;

        svcSleepThread(ticktime);

        if(SecondTick >= samplingrate)
        {
            /*
            char outputbuf[2000], *put = outputbuf;
            for(short i = 0; i<60; i++)
                put += snprintf(put, sizeof outputbuf - (put - outputbuf), "\n%.1f%%-%.1fMHz", GPUUsageHistory[i], GPUClkHistory[i]);
            FileUtils::LogLine("%s", outputbuf);
            */
            SecondTick = 0;
            uint32_t GetConfId = 0;
            apmExtGetCurrentPerformanceConfiguration(&GetConfId);
            SysClkProfile GetProfile = Clocks::GetCurrentProfile();
            if(GetConfId != CurConfId || GetProfile != CurProfile)
            {
                CurConfId = GetConfId;
                CurProfile = GetProfile;
                switch(CurConfId)
                {
                    case 0x92220009:
                    case 0x9222000A:
                    case 0x9222000B:
                    case 0x9222000C:
                        Clocks::ResetToStock();
                        do
                        {
                            CPUTick = 0;
                            svcSleepThread(1'000'000'000);
                            apmExtGetCurrentPerformanceConfiguration(&GetConfId);
                        }
                        while(CurConfId == GetConfId);
                        break;
                    default:
                        SetTable(CurConfId, CurProfile);
                }
            }
        }

        if(CPUTick >= 4)
        {
            CPUTick = 0;
            float CPULoadAvg = 0.4*CPUCoreUsage[0]+0.3*CPUCoreUsage[1]+0.3*CPUCoreUsage[2];
            float CPULoadMax = MAX(MAX(CPUCoreUsage[0], CPUCoreUsage[1]), MAX(CPUCoreUsage[2], CPUCoreUsage[3]));
            float CPULoad = MAX(CPULoadAvg, CPULoadMax);
            for(short i = 0; i < 9*2; i += 2)
            {
                if(CPULoad < *(CPUTable + i + 1))
                {
                    static uint32_t CPUClk;
                    CPUClk = *(CPUTable + i);
                    Clocks::SetHz(SysClkModule_CPU, CPUClk);
                    break;
                }
            }
        }
    }
}

void GovernorInit()
{
    /*
    svcSleepThread(5'000'000'000);
    struct timespec before, after;
    clock_gettime(CLOCK_REALTIME, &before);
    for (int i = 0; i < 10000; i++)
    {
            Clocks::SetHz(SysClkModule_CPU, 612000000);
            Clocks::SetHz(SysClkModule_CPU, 1020000000);
    }
    clock_gettime(CLOCK_REALTIME, &after);
    uint64_t interval = (after.tv_sec - before.tv_sec) * 1'000'000'000 + after.tv_nsec - before.tv_nsec;
    FileUtils::LogLine("10000 Loop interval: %llu ns", interval);
    */
    apmExtGetCurrentPerformanceConfiguration(&CurConfId);
    CurProfile = Clocks::GetCurrentProfile();
    SetTable(CurConfId, CurProfile);
    nvInitialize();
    threadCreate(&Core0, CheckCore0, NULL, NULL, 0x1000, 0x20, 0);
    threadCreate(&Core1, CheckCore1, NULL, NULL, 0x1000, 0x20, 1);
    threadCreate(&Core2, CheckCore2, NULL, NULL, 0x1000, 0x20, 2);
    threadCreate(&Core3, CheckCore3, NULL, NULL, 0x1000, 0x20, 3);
    threadCreate(&GPU, CheckGPU_Set, NULL, NULL, 0x4000, 0x3F, 3);
    threadStart(&Core0);
    threadStart(&Core1);
    threadStart(&Core2);
    threadStart(&Core3);
    threadStart(&GPU);
}

void GovernorExit()
{
    checkexit = true;
    threadWaitForExit(&Core0);
    threadWaitForExit(&Core1);
    threadWaitForExit(&Core2);
    threadWaitForExit(&Core3);
    threadWaitForExit(&GPU);
    threadClose(&Core0);
    threadClose(&Core1);
    threadClose(&Core2);
    threadClose(&Core3);
    threadClose(&GPU);
    nvClose(fd);
    nvExit();
    checkexit = false;
}
