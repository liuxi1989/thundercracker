/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker firmware
 *
 * Copyright <c> 2013 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "pause.h"
#include "bits.h"
#include "macros.h"
#include "tasks.h"
#include "homebutton.h"
#include "led.h"
#include "ledsequencer.h"
#include "cube.h"
#include "event.h"
#include "svmloader.h"
#include "svmclock.h"
#include "batterylevel.h"
#include "btprotocol.h"
#include "ui_shutdown.h"
#include "audiomixer.h"

BitVector<Pause::NUM_WORK_ITEMS> Pause::taskWork;

void Pause::task()
{
    /*
     * The system is paused for
     */

    BitVector<NUM_WORK_ITEMS> pending = taskWork;
    unsigned index;

    while (pending.clearFirst(index)) {
        taskWork.atomicClear(index);
        switch (index) {

        case ButtonPress:
            onButtonChange();
            break;

        case ButtonHold:
            monitorButtonHold();
            break;

        case BluetoothPairing:
            mainLoop(ModeBluetoothPairing);
            break;

        case LowBattery:
            mainLoop(ModeLowBattery);
            break;
        }
    }
}

void Pause::onButtonChange()
{
    /*
     * Never display the pause menu within the launcher.
     * Instead, send it a GameMenu event to indicate the button was pressed.
     * Continue triggering the pause task until button is either released
     * or we reach the shutdown threshold.
     */
    if (SvmLoader::getRunLevel() == SvmLoader::RUNLEVEL_LAUNCHER) {

        if (HomeButton::isPressed()) {
            // start monitoring button hold for shutdown
            Event::setBasePending(Event::PID_BASE_GAME_MENU);
            taskWork.atomicMark(ButtonHold);
            Tasks::trigger(Tasks::Pause);
        }
        return;
    }

    /*
     * Game is running, and button was pressed - execute the pause menu.
     */
    if (HomeButton::isPressed())
        mainLoop(ModePause);
}

void Pause::monitorButtonHold()
{
    /*
     * Shutdown on button press threshold.
     *
     * We only need to monitor this asynchronously because we want to allow
     * the launcher to keep running, and respond to our initial button press
     * in case it needs to unpair a cube (or do anything else) before the
     * shutdown timeout is reached.
     *
     * Pause contexts that the use the firmware UI loop can monitor
     * button hold in that loop.
     */
    if (HomeButton::pressDuration() > SysTime::msTicks(1000)) {
        uint32_t excludedTasks = prepareToPause();
        UICoordinator uic(excludedTasks);
        UIShutdown uiShutdown(uic);
        uiShutdown.init();
        return uiShutdown.mainLoop();
    }

    if (HomeButton::isPressed()) {
        // continue monitoring
        taskWork.atomicMark(ButtonHold);
        Tasks::trigger(Tasks::Pause);
    }
}

uint32_t Pause::prepareToPause()
{
    /*
     * Common operations for entering pause mode,
     * either via mainLoop() or when shutting down.
     */

    if (!SvmClock::isPaused())
        SvmClock::pause();

    /*
     * On hardware, the DAC driver's DMA channel is configured
     * in circular mode, so we need to clear its contents,
     * since we won't be running Tasks::AudioPull while we're
     * paused, which is the mechanism that would normally drain it.
     *
     * We potentially lose any data already written but not yet
     * rendered, but we don't have a way to pause the DMA channel
     * without losing its position.
     */

    AudioMixer::instance.fadeOut();
    while (!AudioMixer::instance.outputBufferIsSilent()) {
        Tasks::work(Intrinsic::LZ(Tasks::Pause));
    }

    uint32_t excludedTasks = Intrinsic::LZ(Tasks::AudioPull) |
                             Intrinsic::LZ(Tasks::Pause);
    return excludedTasks;
}

void Pause::mainLoop(Mode mode)
{
    /*
     * Run the pause menu in the given mode.
     *
     * The pause menu can transition between modes - each
     * mode's handler can indicate whether we should exit from
     * the entire pause context, or whether the mode has changed.
     *
     * Upon mode change, we just need to be sure to init the UI element
     * for that mode.
     *
     * Mode handlers are responsible for detecting their completion
     * conditions, animating the UI, and ensuring that the UI is
     * being shown on a connected cube.
     */

    uint32_t excludedTasks = prepareToPause();
    UICoordinator uic(excludedTasks);

    // all possible UI elements
    UIPause uiPause(uic);
    UICubeRange uiCubeRange(uic);
    UILowBatt uiLowBatt(uic);
    UIBluetoothPairing uiBluetoothPairing(uic, BTProtocol::getPairingCode());

    LED::set(LEDPatterns::paused, true);

    bool finished = false;
    Mode lastMode = static_cast<Mode>(0xff);  // garbage value forces an init

    /*
     * Run our loop, detecting mode changes and pumping the
     * appropriate UI element.
     */

    while (!finished) {

        bool modeChanged = lastMode != mode;
        lastMode = mode;
        uic.stippleCubes(uic.connectCubes());

        switch (mode) {

        case ModePause:
            if (modeChanged && uic.isAttached())
                uiPause.init();
            finished = pauseModeHandler(uic, uiPause, mode);
            break;

        case ModeCubeRange:
            if (modeChanged && uic.isAttached())
                uiCubeRange.init();
            finished = cubeRangeModeHandler(uic, uiCubeRange, mode);
            break;

        case ModeLowBattery:
            if (modeChanged && uic.isAttached())
                uiLowBatt.init();
            finished = lowBatteryModeHandler(uic, uiLowBatt, mode);
            break;

        case ModeBluetoothPairing:
            if (modeChanged && uic.isAttached())
                uiBluetoothPairing.init();
            finished = bluetoothPairingModeHandler(uic, uiBluetoothPairing, mode);
            break;
        }

        // Long press - always allow shut down
        if (HomeButton::pressDuration() > SysTime::msTicks(1000)) {
            UIShutdown uiShutdown(uic);
            uiShutdown.init();
            return uiShutdown.mainLoop();
        }
    }
}

bool Pause::pauseModeHandler(UICoordinator &uic, UIPause &uip, Mode &mode)
{
    if (uic.pollForAttach())
        uip.init();

    uip.animate();
    uic.paint();

    // has menu finished, and button is not still potentially being held for shutdown?
    if (uip.isDone() && HomeButton::isReleased()) {
        cleanup(uic);
        uip.takeAction();
        if (BatteryLevel::needWarning()) {
            mode = ModeLowBattery;
            return false;
        }
        else
            return true;
    }

    // Did we transition back to having too few cubes?
    if (CubeSlots::belowCubeRange())
        mode = ModeCubeRange;

    return false;
}

bool Pause::cubeRangeModeHandler(UICoordinator &uic, UICubeRange &uicr, Mode &mode)
{
    if (uic.pollForAttach())
        uicr.init();

    uicr.animate();
    uic.paint();

    // has menu finished, and button is not still potentially being held for shutdown?
    if (uicr.quitWasSelected() && HomeButton::isReleased()) {
        cleanup(uic);
        SvmLoader::exit();
        return true;
    }

    if (!CubeSlots::belowCubeRange()) {
        /*
         * CubeRange is now fulfilled.
         *
         * If we're in the launcher, there's no reason to pause, so just resume it.
         * Otherwise, transition to pause mode to give the user a chance
         * to gather their thoughts before resuming their game.
         */
        if (SvmLoader::getRunLevel() == SvmLoader::RUNLEVEL_LAUNCHER) {
            cleanup(uic);
            return true;
        }

        if (BatteryLevel::needWarning())
            mode = ModeLowBattery;
        else
            mode = ModePause;
    }

    return false;
}

bool Pause::lowBatteryModeHandler(UICoordinator &uic, UILowBatt &uilb, Mode &mode)
{
    if (uic.pollForAttach())
        uilb.init();

    uilb.animate();
    uic.paint();

    // has menu finished ?
    if (uilb.isDone()) {
        BatteryLevel::setWarningDone();
        cleanup(uic);

        if (uilb.quitWasSelected())
            SvmLoader::exit();
        return true;
    }

    // Did we transition back to having too few cubes?
    if (CubeSlots::belowCubeRange()) {
        mode = ModeCubeRange;
        return false;
    }

    // If a cuberange warning happens it will go to pause too.
    if (HomeButton::isPressed()) {
        BatteryLevel::setWarningDone();
        mode = ModePause;
        return false;
    }

    return false;
}

void Pause::cleanup(UICoordinator &uic)
{
    /*
     * Helper for common clean up tasks when transitioning
     * out of the pause menu altogether.
     */

    uic.restoreCubes(uic.uiConnected);
    LED::set(LEDPatterns::idle);
    Tasks::cancel(Tasks::Pause);
    if (SvmClock::isPaused())
        SvmClock::resume();
}

bool Pause::bluetoothPairingModeHandler(UICoordinator &uic, UIBluetoothPairing &uibp, Mode &mode)
{
    // We can stop remembering to do this mode...
    taskWork.atomicClear(BluetoothPairing);

    if (uic.pollForAttach())
        uibp.init();

    uic.paint();

    // Is pairing finished?
    if (!BTProtocol::isPairingInProgress()) {
        cleanup(uic);
        return true;
    }

    // Home button will dismiss the pairing UI even if it isn't done.
    if (HomeButton::isPressed()) {
        cleanup(uic);
        return true;
    }

    return false;
}

void Pause::beginBluetoothPairing()
{
    /*
     * The lifespan of our Bluetooth pairing UI is canonically controlled
     * by BTProtocol::isPairingInProgress(), but this entry point gives us
     * a way to switch modes and enter the UI.
     *
     * Callable from ISR or Task context.
     */

    taskWork.atomicMark(BluetoothPairing);
    Tasks::trigger(Tasks::Pause);
}
