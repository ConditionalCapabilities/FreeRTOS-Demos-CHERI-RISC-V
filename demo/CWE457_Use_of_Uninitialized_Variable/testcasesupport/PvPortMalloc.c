

#include <stdlib.h>
#include <cheric.h>
#include "FreeRTOS.h"
#include "task.h"



void * pvPortMallocUserSpace( size_t xWantedSize)
{
    void *pvReturn = pvPortMalloc(xWantedSize); 
    pvReturn = cheri_csetopbounds(pvReturn, 0); 
    return pvReturn; 

}