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

#include <interfaces/radio.h>
#include <string.h>
#include <rtx.h>
#include <OpMode_FM.hpp>
#include <OpMode_M17.hpp>

pthread_mutex_t *cfgMutex;      // Mutex for incoming config messages,输入配置消息互斥锁

const rtxStatus_t *newCnf;      // Pointer for incoming config messages，传入rtx驱动状态的指针
rtxStatus_t rtxStatus;          // RTX driver status，RTX驱动状态

float rssi;                     // Current RSSI in dBm，RSSI单位
bool  reinitFilter;             // Flag for RSSI filter re-initialisation，RSSI过滤重新初始化标志

OpMode  *currMode;              // Pointer to currently active opMode handler，指向当前活动opMode处理程序
OpMode     noMode;              // Empty opMode handler for opmode::NONE
OpMode_FM  fmMode;              // FM mode handler    ，Fm模式
OpMode_M17 m17Mode;             // M17 mode handler，M17模式

void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access，拿到没有未上锁的锁
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    currMode = &noMode;

    /*
     * Initialise low-level platform-specific driver,初始化低级别平台驱动
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter，rssi过滤得到的初始值
     */
    rssi         = radio_getRssi();//获取无线电工作状态
    reinitFilter = false;          //rssi重新过滤的标志位
}
/*关闭 RTX 模块，禁用当前操作模式，清理资源，以便程序能够正常地终止。*/
void rtx_terminate()
{
    rtxStatus.opStatus = OFF;             
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    radio_terminate();
}
/*检查配置新的状态。*/
void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);/*上锁保证原子性*/
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

void rtx_task()
{
    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)                           /*上锁函数，成功返回0，但是不会堵塞锁的状态*/
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags
            uint8_t tmp = rtxStatus.opStatus;                           /*运行状态tmp保存*/
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));            /*更新配置*/
            rtxStatus.opStatus = tmp;

            reconfigure = true;                                         /*表示配置已更新*/
            newCnf = NULL;                                              /*清空临时配置函数*/
        }

        pthread_mutex_unlock(cfgMutex);                                 /*释放锁*/
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)                               /*如果不为FM模式则关闭tx和rx*/
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)/*检查当前活动的操作模式是否与新配置的操作模式不同*/
        {
            // Forward opMode change also to radio driver
            radio_setOpmode(static_cast< enum opmode >(rtxStatus.opMode));

            currMode->disable();
            rtxStatus.opStatus = OFF;          /*失能当前的操作模式，将操作状态（opStatus）设置为 "OFF"。*/

            switch(rtxStatus.opMode)
            {
                case OPMODE_NONE: currMode = &noMode;  break;
                case OPMODE_FM:   currMode = &fmMode;  break;
                case OPMODE_M17:  currMode = &m17Mode; break;
                default:   currMode = &noMode;
            }

            currMode->enable();
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)/*只有在接收状态下才会更新rssi*/
    {

        if(!reconfigure)
        {
            if(!reinitFilter)
            {
                rssi = 0.74*radio_getRssi() + 0.26*rssi;
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);/*获取最新的rssi*/
}

float rtx_getRssi()
{
    return rssi;
}

bool rtx_rxSquelchOpen()
{
    return currMode->rxSquelchOpen();
}
