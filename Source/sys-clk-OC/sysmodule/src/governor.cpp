#include "governor.h"
#include "clocks.h"

#define NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD 0x80044715
#define MAXPERCENT  101
#define MAX(a,b)    ((a) > (b) ? (a) : (b))
#define MIN(a,b)    ((a) > (b) ? (b) : (a))
#define IDLETICK(LoadPerc)  (uint32_t)((double)(1.0 - (double)(LoadPerc)/100.0) * (systemtickfreq))
#define GPUTHREASHOLD(GPUPerc) (uint32_t)((GPUPerc) * 10)
#define GPULoadDegradeThre  GPUTHREASHOLD(50)
#define GPULoadDegradeCoef  0.85

const uint32_t samplingrate = 60;
const uint64_t ticktime = 1'000'000'000 / samplingrate;
const uint64_t systemtickfreq = 19'200'000 / (samplingrate / 2);

const uint32_t CPUHand[7][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(50)},
    {816000000,  IDLETICK(60)},
    {918000000,  IDLETICK(70)},
    {1020000000, IDLETICK(80)},
    {1122000000, IDLETICK(95)},
    {1224000000, 0},
};

const uint32_t CPUCharge[8][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(50)},
    {816000000,  IDLETICK(60)},
    {918000000,  IDLETICK(70)},
    {1020000000, IDLETICK(80)},
    {1122000000, IDLETICK(90)},
    {1224000000, IDLETICK(95)},
    {1785000000, 0},
};

const uint32_t CPUDock[9][2] =
{
    {612000000,  IDLETICK(20)},
    {714000000,  IDLETICK(40)},
    {816000000,  IDLETICK(50)},
    {918000000,  IDLETICK(60)},
    {1020000000, IDLETICK(70)},
    {1122000000, IDLETICK(80)},
    {1224000000, IDLETICK(90)},
    {1785000000, IDLETICK(95)},
    {2295000000, 0},
};

const uint32_t GPU230_4[4][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(30)},
    {230400000, GPUTHREASHOLD(50)},
    {307200000, GPUTHREASHOLD(95)},
};

const uint32_t GPU307_2[5][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(25)},
    {230400000, GPUTHREASHOLD(35)},
    {307200000, GPUTHREASHOLD(55)},
    {384000000, GPUTHREASHOLD(95)},
};

const uint32_t GPU384[6][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(20)},
    {230400000, GPUTHREASHOLD(40)},
    {307200000, GPUTHREASHOLD(50)},
    {384000000, GPUTHREASHOLD(70)},
    {460800000, GPUTHREASHOLD(95)},
};

const uint32_t GPU460_8[7][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(20)},
    {230400000, GPUTHREASHOLD(30)},
    {307200000, GPUTHREASHOLD(40)},
    {384000000, GPUTHREASHOLD(50)},
    {460800000, GPUTHREASHOLD(70)},
    {537600000, GPUTHREASHOLD(95)},
};

const uint32_t GPUCharge[9][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(20)},
    {230400000, GPUTHREASHOLD(30)},
    {307200000, GPUTHREASHOLD(40)},
    {384000000, GPUTHREASHOLD(50)},
    {460800000, GPUTHREASHOLD(70)},
    {537600000, GPUTHREASHOLD(80)},
    {691200000, GPUTHREASHOLD(90)},
    {768000000, GPUTHREASHOLD(95)},
};

const uint32_t GPU768[11][2] =
{
    {76800000,  0},
    {153600000, GPUTHREASHOLD(5)},
    {230400000, GPUTHREASHOLD(10)},
    {307200000, GPUTHREASHOLD(20)},
    {384000000, GPUTHREASHOLD(30)},
    {537600000, GPUTHREASHOLD(40)},
    {691200000, GPUTHREASHOLD(50)},
    {768000000, GPUTHREASHOLD(60)},
    {921600000, GPUTHREASHOLD(95)},
    {1075200000,GPUTHREASHOLD(98)},
    {1267200000,GPUTHREASHOLD(99)},
};

Thread Core0, Core1, Core2, Core3, GPU;
bool checkexit = false;
uint32_t fd = 0;

//Adjusted to eliminate float point calculation
uint64_t idletick_a[4] = {0}, idletick_b[4] = {0}, idletick[4] = {0};
uint64_t CPUIdleTickMin = 0;
uint32_t GPULoad = 0;
uint8_t SetHzTick = 0, CoreTick = 0, SecondTick = samplingrate;

const uint32_t* CPUTable;
const uint32_t* GPUTable;

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
            GPUTable = &GPU768[10][1];
            break;
        case SysClkProfile_HandheldChargingOfficial:
            CPUTable = &CPUCharge[7][1];
            GPUTable = &GPUCharge[8][1];
            break;
        default:
            CPUTable = &CPUHand[6][1];
            switch(ConfId)
            {
                case 0x00020000:
                case 0x00020002:
                    GPUTable = &GPU230_4[3][1];
                    break;
                case 0x00020001:
                case 0x00020003:
                case 0x00020005:
                    GPUTable = &GPU307_2[4][1];
                    break;
                case 0x00010000:
                case 0x00020004:
                case 0x00020006:
                    GPUTable = &GPU384[5][1];
                    break;
                case 0x92220007:
                case 0x92220008:
                    GPUTable = &GPU460_8[6][1];
                    break;
                /*
                case 0x00010001:
                case 0x00010002:
                    GPUTable = &GPU768[10][1];
                    break;
                */
                default:
                    GPUTable = &GPU384[5][1];
            }
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
        //GPU Governor: conservative style
        //Sample rate = 60 Hz, SetHz rate = 30 Hz
        //Smooth out GPULoad read (it's common to see 99.7% 99.6% 0.0% 0.0% 99.3% ...)
        //bump up freq instantly if GPULoad is high; gradually drop freq if GPULoad is lower

        //CPU Governor: ondemand style
        //Sample rate = 30 Hz, SetHz rate = 30 Hz

        static uint32_t CurGPULoad;
        rc = nvIoctl(fd, NVGPU_GPU_IOCTL_PMU_GET_GPU_LOAD, &CurGPULoad);
        if (R_FAILED(rc))
            FileUtils::LogLine("[gov] nvIoctl(...): %lx", rc);
        SetHzTick++;
        GPULoad = MAX(CurGPULoad, GPULoad);

        if(SetHzTick % 2 == 0)
        {
            for(unsigned short i = 0; i < 11 * 2; i += 2)
            {
                if(GPULoad > *(GPUTable - i))
                {
                    static uint32_t GPUClk;
                    GPUClk = *(GPUTable - i - 1);
                    Clocks::SetHz(SysClkModule_GPU, GPUClk);
                    break;
                }
            }

            SetHzTick = 0;
            if(GPULoad > GPULoadDegradeThre)
                GPULoad = GPULoad * GPULoadDegradeCoef;
            else
                GPULoad = 0;

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
                switch(CurConfId)
                {
                    case 0x92220009:
                    case 0x9222000A:
                    case 0x9222000B:
                    case 0x9222000C:
                        Clocks::ResetToStock();
                        do
                        {
                            svcSleepThread(1'000'000'000);
                            apmExtGetCurrentPerformanceConfiguration(&GetConfId);
                        }
                        while(CurConfId == GetConfId);
                        CoreTick = 0;
                        break;
                    default:
                        SetTable(CurConfId, CurProfile);
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
