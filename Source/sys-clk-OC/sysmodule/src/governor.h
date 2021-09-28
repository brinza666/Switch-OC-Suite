#pragma once
#include "clock_manager.h"
#include "file_utils.h"
#include <switch.h>

class Governor
{
  public:
    static void Init();
	  static void Exit();
  protected:
    static void Core3StuckCheck();
    static void CheckCore(uint8_t CoreID);
    static void CheckCore0(void*), CheckCore1(void*), CheckCore2(void*), CheckCore3(void*);
    static void Main(void*);
    static void SetTable(uint32_t ConfID, SysClkProfile ConfProfile);
};