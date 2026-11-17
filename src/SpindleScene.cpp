// Copyright (c) 2026 - FluidDial contributors
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// Spindle control: start / stop the spindle in either direction at a chosen RPM.
// Commands are only sent while the machine is Idle; overrides during a job
// are left to the sender.

#include "Scene.h"
#include "System.h"

// Upper limit for the RPM selector. FluidNC clamps to the spindle's own
// speed_map anyway; this only bounds the dial. Override in platformio.local.ini
// with e.g. -DSPINDLE_MAX_RPM=30000
#ifndef SPINDLE_MAX_RPM
#    define SPINDLE_MAX_RPM 24000
#endif
#ifndef SPINDLE_DEFAULT_RPM
#    define SPINDLE_DEFAULT_RPM 10000
#endif

static const int spindle_steps[]  = { 10, 100, 1000 };
static const int n_spindle_steps  = sizeof(spindle_steps) / sizeof(spindle_steps[0]);

// Layout, in the shared 240x240 scene coordinate space
static const int status_y   = 72;
static const int rpm_x      = 24;
static const int rpm_y      = 92;
static const int rpm_w      = 192;
static const int rpm_h      = 44;
static const int step_y     = 148;
static const int dir_x      = 30;
static const int dir_y      = 162;
static const int dir_w      = 180;
static const int dir_h      = 30;

class SpindleScene : public Scene {
private:
    int  _rpm        = SPINDLE_DEFAULT_RPM;
    int  _dir        = SpindleCW;  // Direction used for the next start
    int  _step_index = 1;          // 100 RPM per click
    bool _dirty      = false;      // Prefs need saving

    bool spindleOn() { return mySpindleState == SpindleCW || mySpindleState == SpindleCCW; }

    void startSpindle(int dir) {
        send_linef("%s S%d", dir == SpindleCCW ? "M4" : "M3", _rpm);
        fnc_realtime(StatusReport);
    }

    void savePrefs() {
        if (_dirty) {
            setPref("RPM", _rpm);
            setPref("Dir", _dir);
            setPref("Step", _step_index);
            _dirty = false;
        }
    }

public:
    SpindleScene() : Scene("Spindle", 4) {}

    void onEntry(void* arg) override {
        if (initPrefs()) {
            getPref("RPM", &_rpm);
            getPref("Dir", &_dir);
            getPref("Step", &_step_index);
        }
        if (_rpm < 0 || _rpm > SPINDLE_MAX_RPM) {
            _rpm = SPINDLE_DEFAULT_RPM;
        }
        if (_dir != SpindleCW && _dir != SpindleCCW) {
            _dir = SpindleCW;
        }
        if (_step_index < 0 || _step_index >= n_spindle_steps) {
            _step_index = 1;
        }
        // If the spindle is already turning, start from what it is doing
        if (spindleOn()) {
            _dir = mySpindleState;
            if (mySpeed > 0 && (int)mySpeed <= SPINDLE_MAX_RPM) {
                _rpm = mySpeed;
            }
        }
        fnc_realtime(StatusReport);
    }

    void onExit() override { savePrefs(); }

    void onDialButtonPress() override { pop_scene(); }

    void onGreenButtonPress() override {
        if (state != Idle) {
            return;
        }
        // When running, re-issue in the current direction to apply the new RPM
        startSpindle(spindleOn() ? mySpindleState : _dir);
        ackBeep();
    }

    void onRedButtonPress() override {
        if (state != Idle || !spindleOn()) {
            return;
        }
        send_line("M5");
        fnc_realtime(StatusReport);
        ackBeep();
    }

    void onEncoder(int delta) override {
        if (delta == 0 || state != Idle) {
            return;
        }
        int step = spindle_steps[_step_index];
        // Snap to a multiple of the step so 10 -> 100 -> 1000 changes feel natural
        int rpm = (_rpm / step) * step + delta * step;
        if (delta < 0 && _rpm % step) {
            rpm += step;
        }
        if (rpm < 0) {
            rpm = 0;
        }
        if (rpm > SPINDLE_MAX_RPM) {
            rpm = SPINDLE_MAX_RPM;
        }
        if (rpm != _rpm) {
            _rpm   = rpm;
            _dirty = true;
            reDisplay();
        }
    }

    void onTouchClick() override {
        if (state != Idle) {
            return;
        }
        if (touchY >= rpm_y && touchY < rpm_y + rpm_h + 16) {
            rotateNumberLoop(_step_index, 1, 0, n_spindle_steps - 1);
            _dirty = true;
        } else if (touchY >= dir_y && touchY < dir_y + dir_h && !spindleOn()) {
            // Direction is locked while turning: reversing a spinning
            // spindle is hard on VFDs and tooling.  Stop first.
            _dir   = _dir == SpindleCW ? SpindleCCW : SpindleCW;
            _dirty = true;
        } else {
            return;
        }
        ackBeep();
        reDisplay();
    }

    void onStateChange(state_t old_state) override { reDisplay(); }
    void onDROChange() override { request_redisplay(); }  // Every status report

    void reDisplay() override {
        background();
        drawMenuTitle(current_scene->name());
        drawStatus();

        // Actual spindle state, as reported by FluidNC
        char buf[32];
        switch (mySpindleState) {
            case SpindleCW:
            case SpindleCCW:
                snprintf(buf, sizeof(buf), "%s  %u RPM", mySpindleState == SpindleCW ? "CW" : "CCW", (unsigned)mySpeed);
                centered_text(buf, status_y, GREEN, SMALL);
                break;
            case SpindleOff:
                centered_text("Spindle Off", status_y, LIGHTGREY, SMALL);
                break;
            default:
                centered_text("Spindle ?", status_y, DARKGREY, SMALL);
                break;
        }

        const char* red   = "";
        const char* green = "";

        if (state == Idle) {
            Stripe rpm(rpm_x, rpm_y, rpm_w, rpm_h, MEDIUM);
            rpm.draw("S", intToCStr(_rpm), true);

            snprintf(buf, sizeof(buf), "Step %d  (touch)", spindle_steps[_step_index]);
            centered_text(buf, step_y, LIGHTGREY, TINY);

            bool on  = spindleOn();
            int  dir = on ? mySpindleState : _dir;
            Stripe dirStripe(dir_x, dir_y, dir_w, dir_h, SMALL);
            dirStripe.draw("Dir", dir == SpindleCCW ? "CCW (M4)" : "CW (M3)", !on, on ? DARKGREY : WHITE);

            if (on) {
                red   = "Stop";
                green = "Set";
            } else {
                green = "Start";
            }
        } else {
            centered_text("Wait for Idle", 125, WHITE, SMALL);
        }

        drawButtonLegends(red, green, "Back");
        drawError();
        refreshDisplay();
    }
};

SpindleScene spindleScene;
