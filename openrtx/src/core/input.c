/***************************************************************************
 *   Copyright (C) 2020 - 2023 by Federico Amedeo Izzo IU2NUO,             *
 *                                Niccolò Izzo IU2KIN,                     *
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

#include <interfaces/delays.h>
#include <inttypes.h>
#include <stdbool.h>
#include <input.h>

static long long  keyTs[KBD_NUM_KEYS];  // 每次按键的时间戳
static uint32_t   longPressSent;        // 管理长按事件标志，32位管理
static keyboard_t prevKeys = 0;         // 以前的键盘状态

bool input_scanKeyboard(kbd_msg_t *msg)
{
    msg->value     = 0;                 /*清空联合体*/
    bool kbd_event = false;

    keyboard_t keys = kbd_getKeys();    /*获取点击的按键，在mod17有六个按键通过gpio读取*/
    long long now   = getTick();        /*获取时间戳*/

    // 按键发生变化
    if(keys != prevKeys)                
    {
        // 查找新按的键
        keyboard_t newKeys = (keys ^ prevKeys) & keys;

        // 保存时间戳
        for(uint8_t k = 0; k < KBD_NUM_KEYS; k++)
        {
            keyboard_t mask = 1 << k;
            if((newKeys & mask) != 0)
            {
                keyTs[k]       = now;
                longPressSent &= ~mask;
            }
        }

        // New keyboard event
        msg->keys = keys;
        kbd_event = true;
    }
    // 按键的值和之前一样但是keys值又不为0视为长按
    else if(keys != 0)
    {
        // 检查是否保存触发长按的时间戳
        for(uint8_t k = 0; k < KBD_NUM_KEYS; k++)
        {
            keyboard_t mask = 1 << k;

            // 按键被按下且长按定时器结束
            if(((keys & mask) != 0)          &&
               ((longPressSent & mask) == 0) &&
               ((now - keyTs[k]) >= input_longPressTimeout))
            {
                msg->long_press = 1;
                msg->keys       = keys;
                kbd_event       = true;
                longPressSent  |= mask;
            }
        }
    }

    prevKeys = keys;                       /*保存旧状态*/

    return kbd_event;                      /*返回按键事件是否被触发*/
}

bool input_isNumberPressed(kbd_msg_t msg)
{
    return msg.keys & KBD_NUM_MASK;
}

uint8_t input_getPressedNumber(kbd_msg_t msg)
{
    uint32_t masked_input = msg.keys & KBD_NUM_MASK;
    if (masked_input == 0)
        return 0;

    return __builtin_ctz(msg.keys & KBD_NUM_MASK);
}
