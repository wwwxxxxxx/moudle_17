/***************************************************************************
 *   Copyright (C) 2020 - 2023 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN                      *
 *                                Frederik Saraci IU2NRO                   *
 *                                Silvano Seva IU2KWO                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include <hwconfig.h>
#include <pthread.h>
#include <ui.h>
#include <state.h>
#include <threads.h>
#include <graphics.h>
#include <interfaces/platform.h>
#include <interfaces/delays.h>
#include <interfaces/radio.h>
#include <event.h>
#include <rtx.h>
#include <string.h>
#include <utils.h>
#include <input.h>
#include <backup.h>
#ifdef GPS_PRESENT
#include <peripherals/gps.h>
#include <gps.h>
#endif
#include <voicePrompts.h>

#if defined(PLATFORM_TTWRPLUS)
#include <pmu.h>
#endif

/* Mutex for concurrent access to RTX state variable */
pthread_mutex_t rtx_mutex;

/**
 * \internal Thread managing user input and UI，管理用户输入和UI内部线程
 */
void *ui_threadFunc(void *arg)
{
    (void) arg;

    kbd_msg_t   kbd_msg;                      /*32位的键盘输入结构体*/
    rtxStatus_t rtx_cfg = { 0 };              /*rtx驱动状态结构体*/
    bool        sync_rtx = true;              
    long long   time     = 0;

    /* Load initial state and update the UI，加载初始化状态并更新UI*/
    ui_saveState();
    ui_updateGUI();

    /*在渲染UI屏幕之前，保持开机画面一秒*/
    sleepFor(1u, 0u);
    gfx_render();                            /*屏幕和板子型号选择函数来渲染屏幕*/

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();                     /*获取时间*/

        if(input_scanKeyboard(&kbd_msg))      /*获取按键键值*/
        {
            ui_pushEvent(EVENT_KBD, kbd_msg.value); /*第一个参数两种选择状态变化还是按键变化，将事件写入队列*/
        }

        pthread_mutex_lock(&state_mutex);   // 锁定rw对无线电的访问
        ui_updateFSM(&sync_rtx);            // 更新 UI FSM
        ui_saveState();                     // 保存本地副本
        pthread_mutex_unlock(&state_mutex); // Unlock r/w access to radio state，释放锁

        vp_tick();                           // 继续播放语言提升如果有

        // 如果需要同步，使用互斥锁并更新RTX配置
        if(sync_rtx)
        {
            float power = dBmToWatt(state.channel.power);

            pthread_mutex_lock(&rtx_mutex);                   /*给rtx上锁因为更新配置*/
            rtx_cfg.opMode      = state.channel.mode;        
            rtx_cfg.bandwidth   = state.channel.bandwidth;
            rtx_cfg.rxFrequency = state.channel.rx_frequency;
            rtx_cfg.txFrequency = state.channel.tx_frequency;
            rtx_cfg.txPower     = power;
            rtx_cfg.sqlLevel    = state.settings.sqlLevel;
            rtx_cfg.rxToneEn    = state.channel.fm.rxToneEn;
            rtx_cfg.rxTone      = ctcss_tone[state.channel.fm.rxTone];
            rtx_cfg.txToneEn    = state.channel.fm.txToneEn;
            rtx_cfg.txTone      = ctcss_tone[state.channel.fm.txTone];
            rtx_cfg.toneEn      = state.tone_enabled;

            // 启用TX如果通道允许，我们在UI主屏幕
            rtx_cfg.txDisable = state.channel.rx_only || state.txDisable;

            // 复制新的M17CAN，源地址和目标地址
            rtx_cfg.can = state.settings.m17_can;
            rtx_cfg.canRxEn = state.settings.m17_can_rx;
            strncpy(rtx_cfg.source_address,      state.settings.callsign, 10);
            strncpy(rtx_cfg.destination_address, state.settings.m17_dest, 10);

            pthread_mutex_unlock(&rtx_mutex);

            rtx_configure(&rtx_cfg);                              /*给rtx更新配置*/
            sync_rtx = false;                                     /*修改标志位*/
        }

        // 如有必要，更新UI并在屏幕上呈现
        if(ui_updateGUI() == true)
        {
            gfx_render();
        }

        // 40Hz 键盘和UI刷新数为这么多
        time += 25;
        sleepUntil(time);/*休眠当前线程*/
    }

    ui_terminate();                                                /* 用于在退出前终止 UI（用户界面）线程。*/
    gfx_terminate();                                               /* 用于在退出前终止图形界面。*/

    return NULL;
}

/**
 * \内部线程管理设备和更新全局状态变量
 */
void *main_thread(void *arg)
{
    (void) arg;

    long long time     = 0;

    while(state.devStatus != SHUTDOWN)
    {
        time = getTick();

        #if defined(PLATFORM_TTWRPLUS)
        pmu_handleIRQ();
        #endif

        //选择是否需要下电
        pthread_mutex_lock(&state_mutex);
        if(platform_pwrButtonStatus() == false)       /*m17默认为true*/
            state.devStatus = SHUTDOWN;
        pthread_mutex_unlock(&state_mutex);

        // Run GPS task
        #if defined(GPS_PRESENT) && !defined(MD3x0_ENABLE_DBG)
        gps_task();
        #endif

        // 运行状态更新任务
        state_task();

        // 5ms运行一次这个循环
        time += 5;
        sleepUntil(time);
    }

    #if defined(GPS_PRESENT)
    gps_terminate();
    #endif

    return NULL;
}

/**
 * \internal Thread for RTX management.检查配置和更新线程
 */
void *rtx_threadFunc(void *arg)
{
    (void) arg;

    rtx_init(&rtx_mutex);                /*将未上锁的锁传递过去，初始化rtx以及低级radio驱动*/

    while(state.devStatus == RUNNING)
    {
        rtx_task();                      /*更新配置包括模式以及对rssi的校准*/
    }

    rtx_terminate();                     /*关闭 RTX 模块，禁用当前操作模式，清理资源，以便程序能够正常地终止。*/

    return NULL;
}

/**
 * \internal This function creates all the system tasks and mutexes.创建所有系统任务和互斥锁
 */
void create_threads()
{
    // Create RTX state mutex，设置互斥锁来管理收发，设置为未锁定状态
    pthread_mutex_init(&rtx_mutex, NULL);

    /* Create rtx radio thread，创建线程并设置线程属性pthread_attr_t用于设置线程属性*/
    pthread_attr_t rtx_attr;
    pthread_attr_init(&rtx_attr);/*初始化这个变量保证没有问题*/
	/*操作系统的选择*/
    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&rtx_attr, RTX_TASK_STKSIZE);
    #else
    void *rtx_thread_stack = malloc(RTX_TASK_STKSIZE * sizeof(uint8_t));       /*创建线程栈的空间*/
    pthread_attr_setstack(&rtx_attr, rtx_thread_stack, RTX_TASK_STKSIZE);      /*创建线程栈*/
    #endif

    #ifdef _MIOSIX
    // Max priority for RTX thread when running with miosix rtos，将收发线程优先级设为最高
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(0);                          /*将rtx线程优先级设为最高*/
    pthread_attr_setschedparam(&rtx_attr, &param);                             /*初始化rtx线程*/
    #endif
	/*创建线程函数，传入线程属性，函数里包括rtx初始化，对状态驱动以及radio模式的检测和切换*/
    pthread_t rtx_thread;
    pthread_create(&rtx_thread, &rtx_attr, rtx_threadFunc, NULL);

    // Create UI thread
    pthread_attr_t ui_attr;
    pthread_attr_init(&ui_attr);

    #ifndef __ZEPHYR__
    pthread_attr_setstacksize(&ui_attr, UI_TASK_STKSIZE); 
    #else
    void *ui_thread_stack = malloc(UI_TASK_STKSIZE * sizeof(uint8_t));          /*创建线程栈的空间，2048*/
    pthread_attr_setstack(&ui_attr, ui_thread_stack, UI_TASK_STKSIZE);          /*创建线程栈*/
    #endif

    pthread_t ui_thread;
    pthread_create(&ui_thread, &ui_attr, ui_threadFunc, NULL);
}
