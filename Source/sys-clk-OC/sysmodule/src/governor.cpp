#include "governor.h"
#include "clocks.h"
#include "errors.h"

#define NOBOOSTONBATTERY  TRUE

static Thread t_Core0, t_Core1, t_Core2, t_Core3, t_Main;
static bool governorExit;
static uint32_t nvField;
static uint64_t idletick_prev[4], idletick_next[4], idletick[4];
static bool isCore3Stuck { false };
static uint32_t *tableCPU, *tableGPUNow, *tableGPUMax;
static SysClkProfile g_Profile;
static uint32_t g_ConfId;

constexpr uint8_t sampleRateGPU = 60;
constexpr uint8_t sampleRateCPU = 30;
constexpr uint8_t sampleRatio = sampleRateGPU / sampleRateCPU;
constexpr uint64_t tickTime = 1'000'000'000 / sampleRateGPU;
constexpr uint64_t sysTickFreq { 19'200'000 / sampleRateCPU };

constexpr uint32_t perc2IdleTick(uint8_t LoadPerc)
{
    return (uint32_t)((double)(1 - (double)(LoadPerc)/100.0) * (sysTickFreq));
}

constexpr bool isCPUBoostON(uint32_t ID)
{
    return (ID == 0x92220009 || ID == 0x9222000A);
}

constexpr uint8_t sampleGPU { 5 };
constexpr uint32_t thresholdCore3Stuck { perc2IdleTick(98) };

#if NOBOOSTONBATTERY
uint32_t tableCPUHand[5][2]
{
    {612000000,  perc2IdleTick(20)},
    {714000000,  perc2IdleTick(50)},
    {816000000,  perc2IdleTick(60)},
    {918000000,  perc2IdleTick(70)},
    {1020000000, perc2IdleTick(80)},
};
#else
uint32_t tableCPUHand[7][2]
{
    {612000000,  perc2IdleTick(20)},
    {714000000,  perc2IdleTick(50)},
    {816000000,  perc2IdleTick(60)},
    {918000000,  perc2IdleTick(70)},
    {1020000000, perc2IdleTick(80)},
    {1122000000, perc2IdleTick(90)},
    {1224000000, perc2IdleTick(95)},
};
#endif

uint32_t tableCPUCharge[8][2]
{
    {612000000,  perc2IdleTick(20)},
    {714000000,  perc2IdleTick(50)},
    {816000000,  perc2IdleTick(60)},
    {918000000,  perc2IdleTick(70)},
    {1020000000, perc2IdleTick(75)},
    {1122000000, perc2IdleTick(80)},
    {1224000000, perc2IdleTick(85)},
    {1785000000, perc2IdleTick(90)},
};

uint32_t tableCPUDock[9][2]
{
    {612000000,  perc2IdleTick(20)},
    {714000000,  perc2IdleTick(40)},
    {816000000,  perc2IdleTick(50)},
    {918000000,  perc2IdleTick(60)},
    {1020000000, perc2IdleTick(70)},
    {1122000000, perc2IdleTick(80)},
    {1224000000, perc2IdleTick(85)},
    {1785000000, perc2IdleTick(90)},
    {2295000000, perc2IdleTick(95)},
};

uint32_t tableGPU[17][3]
{
    //76.8 MHz introduces tremendous lag in Power/HOME Menu overlay
    //So say goodbye to it unless in CpuBoostMode
    //Freq, KeepThreshold, BumpThreshold
    {76800000,         0,      0},
    {153600000,        0,   7500},
    {230400000,     5400,   8000},
    {307200000,     6000,   8000},
    {384000000,     6500,   8500},
    {460800000,     7000,   8500},
    {537600000,     7500,   9000},
    {614400000,     8000,   9000},
    {691200000,     8200,   9000},
    {768000000,     8500,   9500},
    {844800000,     8800,   9500},
    {921600000,     8900,   9500},
    {998400000,     9000,   9800},
    {1075200000,    9100,   9800},
    {1152000000,    9200,   9900},
    {1228800000,    9300,   9900},
    {1267200000,    9400,  10000},
};

void Governor::Core3StuckCheck()
{
    if(!isCore3Stuck && !isCPUBoostON(g_ConfId) && (idletick[3] < thresholdCore3Stuck))
    {
        isCore3Stuck = true;
        threadPause(&t_Main);
        Clocks::SetHz(SysClkModule_CPU, *(tableCPU - 1));
        threadResume(&t_Main);
    }
}

void Governor::CheckCore(uint8_t coreID)
{
    while(governorExit == false)
    {
        svcGetInfo(&idletick_prev[coreID], InfoType_IdleTickCount, INVALID_HANDLE, coreID);
        svcSleepThread(tickTime * sampleRatio);
        svcGetInfo(&idletick_next[coreID], InfoType_IdleTickCount, INVALID_HANDLE, coreID);
        idletick[coreID] = idletick_next[coreID] - idletick_prev[coreID];

        if (idletick[coreID] > sysTickFreq)
            idletick[coreID] = sysTickFreq;

        if(coreID != 3)
            Core3StuckCheck();
        else if(isCore3Stuck)
            isCore3Stuck = false;
    }
}

void Governor::CheckCore0(void*) { CheckCore(0); }
void Governor::CheckCore1(void*) { CheckCore(1); }
void Governor::CheckCore2(void*) { CheckCore(2); }
void Governor::CheckCore3(void*) { CheckCore(3); }

void Governor::SetTable(uint32_t confId, SysClkProfile confProfile)
{
    switch(confProfile)
    {
        case SysClkProfile_Docked:
            tableCPU = &tableCPUDock[8][1];
            tableGPUNow = &tableGPU[9][0];          // 768.0 MHz
            tableGPUMax = Clocks::isMariko ?
                            &tableGPU[16][0] :      // 1267.2 MHz
                            &tableGPU[11][0] ;      // 921.6 MHz(Erista)
            break;
        case SysClkProfile_HandheldChargingOfficial:
            tableCPU = &tableCPUCharge[7][1];
            tableGPUNow = &tableGPU[5][0];          // 460.8 MHz
            tableGPUMax = &tableGPU[9][0];          // 768.0 MHz
            break;
        default:
            #if NOBOOSTONBATTERY
            tableCPU = &tableCPUHand[4][1];
            #else
            tableCPU = &tableCPUHand[6][1];
            #endif
            switch(confId)
            {
                case 0x00020000:
                case 0x00020002:
                    tableGPUNow = &tableGPU[2][0];  // 230.4 MHz
                    #if NOBOOSTONBATTERY
                    tableGPUMax = &tableGPU[2][0];  // 230.4 MHz
                    #else
                    tableGPUMax = &tableGPU[3][0];  // 307.2 MHz
                    #endif
                    break;
                case 0x00020001:
                case 0x00020003:
                case 0x00020005:
                    tableGPUNow = &tableGPU[3][0];  // 307.2 MHz
                    #if NOBOOSTONBATTERY
                    tableGPUMax = &tableGPU[3][0];  // 307.2 MHz
                    #else
                    tableGPUMax = &tableGPU[4][0];  // 384.0 MHz
                    #endif
                    break;
                case 0x00010000:
                case 0x00020004:
                case 0x00020006:
                    tableGPUNow = &tableGPU[4][0];  // 384.0 MHz
                    #if NOBOOSTONBATTERY
                    tableGPUMax = &tableGPU[4][0];  // 384.0 MHz
                    #else
                    tableGPUMax = &tableGPU[5][0];  // 460.8 MHz
                    #endif
                    break;
                case 0x92220007:
                case 0x92220008:
                    tableGPUNow = &tableGPU[5][0];  // 460.8 MHz
                    tableGPUMax = Clocks::isMariko ?
                    #if NOBOOSTONBATTERY
                                  &tableGPU[5][0]:  // 460.8 MHz
                    #else
                                  &tableGPU[6][0]:  // 537.6 MHz
                    #endif
                                  &tableGPU[5][0];  // 460.8 MHz(Erista)
                    break;
                default:
                    FileUtils::LogLine("[gov] Error: SetTable unknown confId: %lx", confId);
                    tableGPUNow = &tableGPU[5][0];  // 460.8 MHz
                    tableGPUMax = &tableGPU[5][0];  // 460.8 MHz
            }
    }
    Clocks::SetHz(SysClkModule_GPU, *(tableGPUNow));
}

void Governor::Main(void*)
{
    nvInitialize();

    Result rc;
    rc = nvOpen(&nvField, "/dev/nvhost-ctrl-gpu");
    if (R_FAILED(rc))
        ERROR_THROW("[gov] nvOpen(...): %lx", rc);

    constexpr uint8_t tickProfileMax { sampleRateGPU / 10 };
    uint32_t cachedCPUFreq, cachedGPUFreq;
    uint32_t loadGPUMax { 0 }, loadGPUAdj { 0 };
    uint8_t tickCPU { 0 }, tickGPU { 0 }, tickProfile { sampleRateGPU };
    bool cacheFreqInvalid { true };

    //CPU Governor: ondemand style
    //Sample rate = 30 Hz, SetHz rate = 30 Hz
    //GPU Governor: conservative style
    //Sample rate = 60 Hz, SetHz rate = 60 Hz
    //Smooth out GPULoad read (it's common to see 99.7% 99.6% 0.0% 0.0% 99.3% ...)
    //bump up freq instantly if GPULoad is high; gradually drop freq if GPULoad is lower
    while(governorExit == false)
    {
        if(tickProfile < tickProfileMax)
        {
            if(cacheFreqInvalid)
            {
                cachedGPUFreq = Clocks::GetCurrentHz(SysClkModule_GPU);

                // Sleep mode detected, wait 10 sec then recheck
                while(!cachedGPUFreq)
                {
                    svcSleepThread(10'000'000'000);
                    cachedGPUFreq = Clocks::GetCurrentHz(SysClkModule_GPU);
                }

                // Boost mode detected
                if(cachedGPUFreq == 76'800'000)
                {
                    tickProfile = tickProfileMax;
                    continue;
                }

                cachedCPUFreq = Clocks::GetCurrentHz(SysClkModule_CPU);
                cacheFreqInvalid = false;
            }

            Result rc;
            uint32_t curGPULoad;
            constexpr uint64_t NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD { 0x80044715 };
            rc = nvIoctl(nvField, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &curGPULoad);
            if (R_FAILED(rc))
                ERROR_THROW("[gov] nvIoctl(...): %lx", rc);

            if(curGPULoad > 20) //Ignore values that <= 2.0%
            {
                loadGPUMax = std::max(loadGPUMax, curGPULoad);
                loadGPUAdj = loadGPUMax * 8 + (loadGPUAdj * 2) / 10;
                tickGPU++;
                if(tickGPU == sampleGPU)
                {
                    tickGPU = 0;
                    loadGPUMax = loadGPUAdj * 0.08;
                }
            }

            if(*(tableGPUNow) != cachedGPUFreq)
            {
                tableGPUNow = tableGPUMax;
                do
                {
                    if(*(tableGPUNow) == cachedGPUFreq)
                        break;

                    if(tableGPUNow != &tableGPU[0][0])
                        tableGPUNow -= 3;

                } while (tableGPUNow >= &tableGPU[0][0]);
            }

            if((*(tableGPUNow + 1) > loadGPUAdj) && (tableGPUNow > &tableGPU[0][0]))
            {
                tableGPUNow -= 3;
                cachedGPUFreq = *(tableGPUNow);
                Clocks::SetHz(SysClkModule_GPU, cachedGPUFreq);
            }
            else if((*(tableGPUNow + 2) < loadGPUAdj) && (tableGPUNow < tableGPUMax))
            {
                tableGPUNow += 3;
                cachedGPUFreq = *(tableGPUNow);
                Clocks::SetHz(SysClkModule_GPU, cachedGPUFreq);
            }

            tickCPU++;
            if(!isCore3Stuck && tickCPU == sampleRatio)
            {
                tickCPU = 0;
                using std::min;
                uint64_t idletickAdj { min(min(idletick[0], idletick[1]),
                                           min(idletick[2], idletick[3])) };

                for(uint8_t i = 0; i < 8 * 2; i += 2)
                {
                    if(idletickAdj <= *(tableCPU - i))
                    {
                        uint32_t clockCPU;
                        clockCPU = *(tableCPU - i - 1);
                        if(clockCPU != cachedCPUFreq)
                        {
                            cachedCPUFreq = clockCPU;
                            Clocks::SetHz(SysClkModule_CPU, clockCPU);
                        }
                        break;
                    }
                }
            }

            svcSleepThread(tickTime);
            tickProfile++;
        }
        else
        {
            tickProfile = 0;
            cacheFreqInvalid = true;
            uint32_t nowConfId = 0;
            apmExtGetCurrentPerformanceConfiguration(&nowConfId);
            SysClkProfile nowProfile = Clocks::GetCurrentProfile();
            if(nowConfId != g_ConfId || nowProfile != g_Profile)
            {
                g_ConfId = nowConfId;
                g_Profile = nowProfile;
                if(isCPUBoostON(g_ConfId))
                {
                    Clocks::ResetToStock();
                    do
                    {
                        svcSleepThread(tickTime * sampleRatio);
                        apmExtGetCurrentPerformanceConfiguration(&g_ConfId);
                    }
                    while(isCPUBoostON(g_ConfId));
                }
                SetTable(g_ConfId, g_Profile);
            }
        }
    }
}

void Governor::Init()
{
    apmExtGetCurrentPerformanceConfiguration(&g_ConfId);
    g_Profile = Clocks::GetCurrentProfile();
    SetTable(g_ConfId, g_Profile);
    threadCreate(&t_Core0, CheckCore0, NULL, NULL, 0x1000, 0x20, 0);
    threadCreate(&t_Core1, CheckCore1, NULL, NULL, 0x1000, 0x20, 1);
    threadCreate(&t_Core2, CheckCore2, NULL, NULL, 0x1000, 0x20, 2);
    threadCreate(&t_Core3, CheckCore3, NULL, NULL, 0x1000, 0x3F, 3);
    threadCreate(&t_Main,  Main,       NULL, NULL, 0x1000, 0x3F, 3);
    threadStart(&t_Core0);
    threadStart(&t_Core1);
    threadStart(&t_Core2);
    threadStart(&t_Core3);
    threadStart(&t_Main);
}

void Governor::Exit()
{
    governorExit = true;
    threadWaitForExit(&t_Core0);
    threadWaitForExit(&t_Core1);
    threadWaitForExit(&t_Core2);
    threadWaitForExit(&t_Core3);
    threadWaitForExit(&t_Main);
    threadClose(&t_Core0);
    threadClose(&t_Core1);
    threadClose(&t_Core2);
    threadClose(&t_Core3);
    threadClose(&t_Main);
    nvClose(nvField);
    nvExit();
    governorExit = false;
}
