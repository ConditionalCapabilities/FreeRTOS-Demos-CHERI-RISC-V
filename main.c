/*
 * FreeRTOS Kernel V10.1.1
 * Copyright (C) 2018 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */

/**
 * This is docs for main
 */

/* FreeRTOS kernel includes. */

#include <time.h>

#include <FreeRTOS.h>
#include <task.h>

#if mainCONFIG_USE_DYNAMIC_LOADER
/* FreeRTOS-libdl includes to dynamically load and link objects */
    #include <dlfcn.h>
    #include <rtl/rtl-trace.h>
    #include <rtl/rtl-archive.h>
#endif

#if mainCONFIG_INIT_FAT_FILESYSTEM
/* FreeRTOS+FAT includes. */
    #include "ff_headers.h"
    #include "ff_stdio.h"

    #if configHAS_VIRTIO_BLK
        #include "ff_virtioblk_disk.h"
    #else
        #include "ff_ramdisk.h"
    #endif
#endif /* mainCONFIG_INIT_FAT_FILESYSTEM */

/* Bsp includes. */
#include "bsp.h"

#ifdef __CHERI_PURE_CAPABILITY__
    #include <cheri_init_globals.h>
    #include <cheri/cheri-utility.h>

    void _start_purecap( void )
    {
        cheri_init_globals_3( __builtin_cheri_global_data_get(),
                              __builtin_cheri_program_counter_get(),
                              __builtin_cheri_global_data_get() );
    }
#endif /* __CHERI_PURE_CAPABILITY__ */

/******************************************************************************
 */
#if mainDEMO_TYPE == 1
    #pragma message "Demo type 1: Basic Blinky"
    extern void main_blinky( void );
#elif mainDEMO_TYPE == 3
    #pragma message "Demo type 3: Tests"
    extern void main_tests( void );
#elif mainDEMO_TYPE == 4
    #pragma message "Demo type 4: Compartment Tests"
    extern void main_compartment_test( void );
#elif mainDEMO_TYPE == 5
    #pragma message "Demo type 5: TCP/IP peekpoke Test"
    extern void main_peekpoke( void );
#elif mainDEMO_TYPE == 6
    #pragma message "Demo type : UDP/TCP/IP-based echo, CLI, HTTP, FTP and TFTP servers"
    extern void main_servers( void );
#elif mainDEMO_TYPE == 42
    #pragma message "Demo type 42: Modbus"
    extern void main_modbus( void );
#else /* if mainDEMO_TYPE == 1 */
    #ifdef configPROG_ENTRY
        extern int configPROG_ENTRY( int argc, char* argv[]);
    #else
        #error "Unsupported demo type"
    #endif
#endif /* if mainDEMO_TYPE == 1 */

/* Prototypes for the standard FreeRTOS callback/hook functions implemented
 * within this file.  See https://www.freertos.org/a00016.html */
void vApplicationMallocFailedHook( void );
void vApplicationIdleHook( void );
void vApplicationStackOverflowHook( TaskHandle_t pxTask,
                                    char * pcTaskName );
void vApplicationTickHook( void );

void vToggleLED( void );

#if __riscv_xlen == 64
    #define read_csr( reg )                                  \
    ( { unsigned long __tmp;                                 \
        __asm__ volatile ( "csrr %0, " # reg : "=r" ( __tmp ) ); \
        __tmp; } )
#endif

/**
 * Capture the current 64-bit cycle count.
 */
uint64_t get_cycle_count( void )
{
    #if __riscv_xlen == 64
        return read_csr( cycle );
    #else
        uint32_t cycle_lo, cycle_hi;
        __asm__ volatile (
            "%=:\n\t"
            "csrr %1, cycleh\n\t"
            "csrr %0, cycle\n\t"
            "csrr t1, cycleh\n\t"
            "bne  %1, t1, %=b"
            : "=r" ( cycle_lo ), "=r" ( cycle_hi )
            : /* No inputs. */
            : "t1" );
        return ( ( ( uint64_t ) cycle_hi ) << 32 ) | ( uint64_t ) cycle_lo;
    #endif /* if __riscv_xlen == 64 */
}

/**
 * Use `mcycle` counter to get usec resolution.
 * On RV32 only, reads of the mcycle CSR return the low 32 bits,
 * while reads of the mcycleh CSR return bits 63–32 of the corresponding
 * counter.
 * We convert the 64-bit read into usec. The counter overflows in roughly an hour
 * and 20 minutes. Probably not a big issue though.
 * At 50HMz clock rate, 1 us = 50 ticks
 */
uint64_t port_get_current_mtime( void )
{
    return ( uint64_t ) ( get_cycle_count() / ( configCPU_CLOCK_HZ / 1000000 ) );
}
/*-----------------------------------------------------------*/

#if mainCONFIG_INIT_FAT_FILESYSTEM

/* The number and size of sectors that will make up the RAM disk.  The RAM disk
 * is huge to allow some verbose FTP tests. */
    #ifndef mainDISK_NAME
        #define mainDISK_NAME            "/"
    #endif

    #define mainRAM_DISK_SECTOR_SIZE     512UL                                                    /* Currently fixed! */

    #ifndef mainRAM_DISK_SECTORS
        #define mainRAM_DISK_SECTORS         ( ( 80UL * 1024UL * 1024UL ) / mainRAM_DISK_SECTOR_SIZE ) /* 80M bytes. */
    #endif /* mainRAM_DISK_SECTORS */

    #define mainIO_MANAGER_CACHE_SIZE    ( 15UL * mainRAM_DISK_SECTOR_SIZE )

    static void prvCreateDisk( void )
    {
        FF_Disk_t * pxDisk;

        /* Create the VirtIO Block Disk. */
        #if configHAS_VIRTIO_BLK
            pxDisk = FF_VirtIODiskInit( mainDISK_NAME, mainIO_MANAGER_CACHE_SIZE );

            #if DEBUG
                /* Print out information on the disk. */
                FF_VirtIODiskShowPartition( pxDisk );
            #endif
        #else
            static uint8_t ucRAMDisk[ mainRAM_DISK_SECTORS * mainRAM_DISK_SECTOR_SIZE ];
            pxDisk = FF_RAMDiskInit( mainDISK_NAME, ucRAMDisk, mainRAM_DISK_SECTORS, (uint32_t) mainIO_MANAGER_CACHE_SIZE );

            #if DEBUG
                /* Print out information on the disk. */
                FF_RAMDiskShowPartition( pxDisk );
            #endif
        #endif /* configHAS_VIRTIO_BLK */

        configASSERT( pxDisk );

    }
#endif /* mainCONFIG_INIT_FAT_FILESYSTEM */

#if mainCONFIG_USE_DYNAMIC_LOADER

    #ifndef configPROG_ENTRY
        #error "configPROG_ENTRY must be defined as the entry function for the application \
    "to which the dynamic loader jumps to"
    #endif

    #ifndef mainCONFIG_INIT_FAT_FILESYSTEM
        #error "The dynamic loader requires a FAT filesystem, use mainCONFIG_INIT_FAT_FILESYSTEM"
    #endif

    #ifndef configFF_FORMATTED_DISK_IMAGE
        void vFatEmbedLibFiles( void );
    #endif

    static void prvLoader( void* unused )
    {
        typedef void (* prog_entry_t)( int argc, char *argv[]);
        prog_entry_t entry = NULL;

        #ifndef configFF_FORMATTED_DISK_IMAGE
        /* Embed the libs in the file systems*/
            vFatEmbedLibFiles();
        #endif

        #if DEBUG

            rtems_rtl_trace_set_mask( RTEMS_RTL_TRACE_UNRESOLVED );

            /* rtems_rtl_trace_set_mask( RTEMS_RTL_TRACE_UNRESOLVED | \
                    RTEMS_RTL_TRACE_WARNING | \
                    RTEMS_RTL_TRACE_LOAD | \
                    RTEMS_RTL_TRACE_DETAIL | \
                    RTEMS_RTL_TRACE_UNLOAD | \
                    RTEMS_RTL_TRACE_SYMBOL | \
                    RTEMS_RTL_TRACE_GLOBAL_SYM | \
                    RTEMS_RTL_TRACE_RELOC | \
                    RTEMS_RTL_TRACE_ALLOCATOR | \
                    RTEMS_RTL_TRACE_UNRESOLVED | \
                    RTEMS_RTL_TRACE_ARCHIVES | \
                    RTEMS_RTL_TRACE_CACHE | \
                    RTEMS_RTL_TRACE_CHERI | \
                    RTEMS_RTL_TRACE_LOAD_SECT | \
                    RTEMS_RTL_TRACE_DEPENDENCY); */
            printf("main.c: Loading %s\n", configLIBDL_PROG_START_OBJ);
        #endif

        void * obj = dlopen( configLIBDL_PROG_START_OBJ, RTLD_GLOBAL | RTLD_NOW );

        if( obj == NULL )
        {
            printf( "Failed to dynamically load the app and/or libs\n" );
            exit( -1 );
        }

    #define str( s )     # s
    #define xstr( s )    str( s )

        #if DEBUG
            printf( "Searching loaded objects for %s\n", xstr( configPROG_ENTRY ) );
        #endif

        entry = ( prog_entry_t ) dlsym( obj, xstr( configPROG_ENTRY ) );

        if( entry == NULL )
        {
            printf( "Failed to to find the specified prog entry: %s\n", xstr( configPROG_ENTRY ) );
            exit( -1 );
        }
        else
        {
            #if DEBUG
                printf( "Found entry %s @ %p\n", xstr( configPROG_ENTRY ), entry );
            #endif
        }

        #if DEBUG
            printf( "Jumping to the app entry: %s\n", xstr( configPROG_ENTRY ) );
        #endif

        vTaskSuspendAll();
        entry(0, NULL);

        if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED)
            xTaskResumeAll();

        while( 1 )
        {
        }
    }
#endif /* if mainCONFIG_USE_DYNAMIC_LOADER */
/*-----------------------------------------------------------*/

int demo_main( void )
{
    prvSetupHardware();

    #if mainCONFIG_INIT_FAT_FILESYSTEM
        prvCreateDisk();
    #endif

    /* The mainCREATE_SIMPLE_BLINKY_DEMO_ONLY setting is described at the top
     * of this file. */
    #if mainDEMO_TYPE == 1
        {
            main_blinky();
        }
    #elif mainDEMO_TYPE == 3
        {
            main_tests();
        }
    #elif mainDEMO_TYPE == 4
        {
            main_compartment_test();
        }
    #elif mainDEMO_TYPE == 5
        {
            main_peekpoke();
        }
    #elif mainDEMO_TYPE == 6
        {
            main_servers();
        }
    #elif mainDEMO_TYPE == 42
        /* run the main_modbus demo */
        {
            main_modbus();
        }
    #else /* if mainDEMO_TYPE == 1 */
        #ifdef configPROG_ENTRY
            #if mainCONFIG_USE_DYNAMIC_LOADER
                /* Create a task that dynamically loads libs and apps from the file system */
                xTaskCreate( prvLoader,
                             "loader",
                             configMINIMAL_STACK_SIZE * 2U,
                             NULL,
                             tskIDLE_PRIORITY | portPRIVILEGE_BIT,
                             NULL );
            #else
                xTaskCreate( ( TaskFunction_t ) configPROG_ENTRY,
                             "root-app",
                             configMINIMAL_STACK_SIZE * 2U,
                             0,
                             tskIDLE_PRIORITY | portPRIVILEGE_BIT,
                             NULL );
            #endif
        #else /* ifdef configPROG_ENTRY */
        #error "Unsupported Demo"
        #endif /* ifdef configPROG_ENTRY */
    #endif /* if mainDEMO_TYPE == 1 */
    vTaskStartScheduler();

    for( ; ; )
    {
    }
}
/*-----------------------------------------------------------*/

/*-----------------------------------------------------------*/
time_t FreeRTOS_time( time_t * pxTime )
{
    time_t xReturn;

    xReturn = time( &xReturn );

    if( pxTime != NULL )
    {
        *pxTime = xReturn;
    }

    return xReturn;
}

/* TODO: toggle a real LED at some point */
void vToggleLED( void )
{
    static uint32_t ulLEDState = 0;

    /*  GPIO_set_outputs( &g_gpio_out, ulLEDState ); */
    ulLEDState = !ulLEDState;
}
/* / *-----------------------------------------------------------* / */

void vApplicationMallocFailedHook( void )
{
    /* vApplicationMallocFailedHook() will only be called if
     * configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
     * function that will get called if a call to pvPortMalloc() fails.
     * pvPortMalloc() is called internally by the kernel whenever a task, queue,
     * timer or semaphore is created.  It is also called by various parts of the
     * demo application.  If heap_1.c or heap_2.c are used, then the size of the
     * heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
     * FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
     * to query the size of free heap space that remains (although it does not
     * provide information on how the remaining heap might be fragmented). */
    taskDISABLE_INTERRUPTS();
    __asm__ volatile ( "ebreak" );

    for( ; ; )
    {
    }
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
    /* vApplicationIdleHook() will only be called if configUSE_IDLE_HOOK is set
     * to 1 in FreeRTOSConfig.h.  It will be called on each iteration of the idle
     * task.  It is essential that code added to this hook function never attempts
     * to block in any way (for example, call xQueueReceive() with a block time
     * specified, or call vTaskDelay()).  If the application makes use of the
     * vTaskDelete() API function (as this demo application does) then it is also
     * important that vApplicationIdleHook() is permitted to return to its calling
     * function, because it is the responsibility of the idle task to clean up
     * memory allocated by the kernel to any task that has since been deleted. */
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask,
                                    char * pcTaskName )
{
    ( void ) pcTaskName;
    ( void ) pxTask;

    /* Run time stack overflow checking is performed if
     * configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
     * function is called if a stack overflow is detected. */
    taskDISABLE_INTERRUPTS();
    __asm__ volatile ( "ebreak" );

    for( ; ; )
    {
    }
}
/*-----------------------------------------------------------*/

void vApplicationTickHook( void )
{
    /* The tests in the full demo expect some interaction with interrupts. */
    #if ( mainDEMO_TYPE == 2 )
        {
            extern void vFullDemoTickHook( void );
            vFullDemoTickHook();
        }
    #endif
}
/*-----------------------------------------------------------*/

void vApplicationTaskExit( void )
{
    vTaskDelete( NULL );
    __builtin_unreachable();
    while ( 1 );
}
/*-----------------------------------------------------------*/

/* configUSE_STATIC_ALLOCATION is set to 1, so the application must provide an
 * implementation of vApplicationGetIdleTaskMemory() to provide the memory that is
 * used by the Idle task. */
void vApplicationGetIdleTaskMemory( StaticTask_t ** ppxIdleTaskTCBBuffer,
                                    StackType_t ** ppxIdleTaskStackBuffer,
                                    uint32_t * pulIdleTaskStackSize )
{
    /* If the buffers to be provided to the Idle task are declared inside this
     * function then they must be declared static - otherwise they will be allocated on
     * the stack and so not exists after this function exits. */
    static StaticTask_t xIdleTaskTCB;
    static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
     * state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
/*-----------------------------------------------------------*/

/* configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
 * application must provide an implementation of vApplicationGetTimerTaskMemory()
 * to provide the memory that is used by the Timer service task. */
void vApplicationGetTimerTaskMemory( StaticTask_t ** ppxTimerTaskTCBBuffer,
                                     StackType_t ** ppxTimerTaskStackBuffer,
                                     uint32_t * pulTimerTaskStackSize )
{
    /* If the buffers to be provided to the Timer task are declared inside this
     * function then they must be declared static - otherwise they will be allocated on
     * the stack and so not exists after this function exits. */
    static StaticTask_t xTimerTaskTCB;
    static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    /* Pass out a pointer to the StaticTask_t structure in which the Timer
     * task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
/*-----------------------------------------------------------*/
