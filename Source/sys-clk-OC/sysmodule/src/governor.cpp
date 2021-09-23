#include "governor.h"
#include "clocks.h"

#define NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD 0x80044715
#define MAX(a,b)    ((a) > (b) ? (a) : (b))
#define MIN(a,b)    ((a) > (b) ? (b) : (a))
#define IDLETICK(LoadPerc)  (uint32_t)((double)(1.0 - (double)(LoadPerc)/100.0) * (systemtickfreq))
#define GPUSample   4

const uint32_t samplingrate = 60;
const uint64_t ticktime = 1'000'000'000 / samplingrate;
const uint64_t systemtickfreq = 19'200'000 / (samplingrate / 2);

uint32_t CPUHand[7][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(50)},
    {816000000,  IDLETICK(60)},
    {918000000,  IDLETICK(70)},
    {1020000000, IDLETICK(80)},
    {1122000000, IDLETICK(90)},
    {1224000000, IDLETICK(95)},
};

uint32_t CPUCharge[8][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(50)},
    {816000000,  IDLETICK(60)},
    {918000000,  IDLETICK(70)},
    {1020000000, IDLETICK(75)},
    {1122000000, IDLETICK(80)},
    {1224000000, IDLETICK(85)},
    {1785000000, IDLETICK(90)},
};

uint32_t CPUDock[9][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(40)},
    {816000000,  IDLETICK(50)},
    {918000000,  IDLETICK(60)},
    {1020000000, IDLETICK(70)},
    {1122000000, IDLETICK(80)},
    {1224000000, IDLETICK(85)},
    {1785000000, IDLETICK(90)},
    {2295000000, IDLETICK(95)},
};

uint32_t GPUTable[17][3] =
{
    //Freq, KeepThreshold, BumpThreshold
    {76800000,      0,      7000},
    {153600000,     4000,   7500},
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
    {1267200000,    9300,   9900},
    {1267200000,    9400,  10000},
};

Thread Core0, Core1, Core2, Core3, GPU;
bool checkexit = false;
uint32_t fd = 0;

//Adjusted to eliminate float point calculation
uint64_t idletick_a[4] = {0}, idletick_b[4] = {0}, idletick[4] = {0};
uint64_t CPUIdleTickMin = 0;
uint32_t GPULoad[4] = { 0 }, GPULoadAdj = 0, GPUMax = 0;
uint8_t GPUTick = 0, CoreTick = 0, SecondTick = samplingrate;
bool SetHzTick = false;

uint32_t *CPUTable;
uint32_t *GPUTablePointer, *GPUTableMax;

SysClkProfile CurProfile;
uint32_t CurConfId;

void CheckCore(uint8_t CoreID)
{
    while(checkexit == false)
    {
        svcGetInfo(&idletick_a[CoreID], InfoType_IdleTickCount, INVALID_HANDLE, CoreID);
        svcSleepThread(ticktime * 2);
        svcGetInfo(&idletick_b[CoreID], InfoType_IdleTickCount, INVALID_HANDLE, CoreID);
        idletick[CoreID] = idletick_b[CoreID] - idletick_a[CoreID];

        if (idletick[CoreID] > systemtickfreq)
            idletick[CoreID] = systemtickfreq;

        CoreTick++;
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
    switch(ConfProfile)
    {
        case SysClkProfile_Docked:
            CPUTable = &CPUDock[8][1];
            GPUTablePointer = &GPUTable[9][0];          // 768.0 MHz
            GPUTableMax = &GPUTable[16][0];             // 1267.2 MHz
            break;
        case SysClkProfile_HandheldChargingOfficial:
            CPUTable = &CPUCharge[7][1];
            GPUTablePointer = &GPUTable[5][0];          // 460.8 MHz
            GPUTableMax = &GPUTable[9][0];              // 768.0 MHz
            break;
        default:
            CPUTable = &CPUHand[6][1];
            switch(ConfId)
            {
                case 0x00020000:
                case 0x00020002:
                    GPUTablePointer = &GPUTable[2][0];  // 230.4 MHz
                    GPUTableMax = &GPUTable[3][0];      // 307.2 MHz
                    break;
                case 0x00020001:
                case 0x00020003:
                case 0x00020005:
                    GPUTablePointer = &GPUTable[3][0];  // 307.2 MHz
                    GPUTableMax = &GPUTable[4][0];      // 384.0 MHz
                    break;
                case 0x00010000:
                case 0x00020004:
                case 0x00020006:
                    GPUTablePointer = &GPUTable[4][0];  // 384.0 MHz
                    GPUTableMax = &GPUTable[5][0];      // 460.8 MHz
                    break;
                case 0x92220007:
                case 0x92220008:
                    GPUTablePointer = &GPUTable[5][0];  // 460.8 MHz
                    GPUTableMax = &GPUTable[6][0];      // 537.6 MHz
                    break;
                default:
                    GPUTablePointer = &GPUTable[4][0];  // 384.0 MHz
                    GPUTableMax = &GPUTable[5][0];      // 460.8MHz
            }
    }
    Clocks::SetHz(SysClkModule_GPU, *(GPUTablePointer));
}

void CheckGPU_Set(void*)
{
    Result rc;
    rc = nvOpen(&fd, "/dev/nvhost-ctrl-gpu");
    if (R_FAILED(rc))
        FileUtils::LogLine("[gov] nvOpen(...): %lx", rc);
    while(checkexit == false)
    {
        //GPU Governor: conservative style
        //Sample rate = 60 Hz, SetHz rate = 60 Hz
        //Smooth out GPULoad read (it's common to see 99.7% 99.6% 0.0% 0.0% 99.3% ...)
        //bump up freq instantly if GPULoad is high; gradually drop freq if GPULoad is lower

        //CPU Governor: ondemand style
        //Sample rate = 30 Hz, SetHz rate = 30 Hz

        static uint32_t CurGPULoad;
        rc = nvIoctl(fd, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &CurGPULoad);
        if (R_FAILED(rc))
            FileUtils::LogLine("[gov] nvIoctl(...): %lx", rc);

        if(CurGPULoad > 20) //Ignore values that <= 2.0%
        {
            GPULoad[GPUTick] = CurGPULoad;
            GPUMax = MAX(MAX(GPULoad[0], GPULoad[1]), MAX(GPULoad[2], GPULoad[3]));
            GPULoadAdj = GPUMax * 6 + (GPULoad[0] + GPULoad[1] + GPULoad[2] + GPULoad[3]);
            GPUTick++;
        }

        if(GPUTick == GPUSample)
        {
            GPUTick = 0;
        }
        
        if((*(GPUTablePointer + 1) > GPULoadAdj) && (GPUTablePointer >= &GPUTable[0][0]))
        {
            GPUTablePointer -= 3;
        }
        else if((*(GPUTablePointer + 2) < GPULoadAdj))
        {
            if(GPUTablePointer == &GPUTable[0][0] || GPUTablePointer == &GPUTable[1][0])
                GPUTablePointer += 6;
            else if(GPUTablePointer < GPUTableMax)
                GPUTablePointer += 3;
        }

        Clocks::SetHz(SysClkModule_GPU, *(GPUTablePointer));

        SetHzTick = !SetHzTick;
        if(SetHzTick)
        {
            CPUIdleTickMin = MIN(MIN(idletick[0], idletick[1]), MIN(idletick[2], idletick[3]));

            for(unsigned short j = 0; j < 8*2; j += 2)
            {
                if(CPUIdleTickMin <= *(CPUTable - j))
                {
                    static uint32_t CPUClk;
                    CPUClk = *(CPUTable - j - 1);
                    Clocks::SetHz(SysClkModule_CPU, CPUClk);
                    break;
                }
            }
        }

        SecondTick++;

        svcSleepThread(ticktime);

        if(SecondTick >= samplingrate)
        {
            SecondTick = 0;
            uint32_t GetConfId = 0;
            apmExtGetCurrentPerformanceConfiguration(&GetConfId);
            SysClkProfile GetProfile = Clocks::GetCurrentProfile();
            if(GetConfId != CurConfId || GetProfile != CurProfile)
            {
                CurConfId = GetConfId;
                CurProfile = GetProfile;
                while(CurConfId == 0x92220009 || CurConfId == 0x9222000A || CurConfId == 0x9222000B || CurConfId == 0x9222000C)
                {
                    Clocks::ResetToStock();
                    svcSleepThread(1'000'000'000);
                    apmExtGetCurrentPerformanceConfiguration(&CurConfId);
                    CoreTick = 0;
                }
                SetTable(CurConfId, CurProfile);
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
