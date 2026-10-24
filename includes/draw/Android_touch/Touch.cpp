#include <cstdio>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <thread>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <cmath>
#include <unistd.h>
#include <vector>
#include "imgui.h"
#include "Android_touch/Touch.hpp"

#define TOUCH_MODE 2

namespace touch {
    struct input {
        int fd;
        bool canBeUsed{false};
        input_absinfo absX{}, absY{};
        float absXMultiplier{0}, absYMultiplier{0};
#if TOUCH_MODE == 2
        bool trackingIDPresent{false};
#endif

        input(const char *_path, int32_t _screen_w, int32_t _screen_h) {
            fd = open(_path, O_RDWR);
            if (fd == -1)
                return;
            canBeUsed = ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absX) != -1 && ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absY) != -1 && absX.maximum > 0 && absY.maximum > 0;
            if (!canBeUsed) {
                close();
                return;
            }

#if TOUCH_MODE == 2
            input_absinfo absTrackingID{};
            trackingIDPresent = ioctl(fd, EVIOCGABS(ABS_MT_TRACKING_ID), &absTrackingID) != -1 && absTrackingID.minimum <= 0 && absTrackingID.maximum > 0;
#endif

            calculateMultipliers(_screen_w, _screen_h);
        }

        void close() {
            if (fd != -1) {
                ::close(fd);
                fd = -1;
            }
        }

        void calculateMultipliers(int32_t _screen_w, int32_t _screen_h) {
            auto max = fmax((float) absX.maximum, (float) absY.maximum);
            auto min = fmin((float) absX.maximum, (float) absY.maximum);
            absXMultiplier = (float) _screen_w / (_screen_w > _screen_h ? max : min);
            absYMultiplier = (float) _screen_h / (_screen_w > _screen_h ? min : max);
        }
    };

    uint8_t orientation;
    ImGuiIO *imgui_io;
    std::vector<input> inputs;

    // ---- every finger on the screen (multi-touch protocol B), in display pixels ---------------------
    // The menu works with the last pointer only; this keeps all of them, so "is a finger on that button"
    // can be asked while the other thumb turns the camera.
    constexpr int kSlots = 16;
    std::mutex g_finger_mutex;
    Finger g_fingers[kSlots];
    int g_finger_count = 0;

    // Raw panel coordinates of one device to display pixels (the same mapping the pointer uses).
    static void to_display(const input &in, float x, float y, float &sx, float &sy) {
        switch (orientation) {
            case 1: sx = y; sy = (float) in.absX.maximum - x; break;
            case 2: sx = (float) in.absX.maximum - x; sy = (float) in.absY.maximum - y; break;
            case 3: sx = (float) in.absY.maximum - y; sy = x; break;
            default: sx = x; sy = y; break;
        }
        sx *= in.absXMultiplier;
        sy *= in.absYMultiplier;
    }

    struct SlotTracker {
        struct Slot { bool down = false; float x = 0.f, y = 0.f; } slots[kSlots];
        int current = 0;
        void feed(const input &in, const input_event &event) {
            if (event.type == EV_ABS) {
                if (event.code == ABS_MT_SLOT) {
                    current = event.value < 0 ? 0 : (event.value >= kSlots ? kSlots - 1 : event.value);
                } else if (event.code == ABS_MT_TRACKING_ID) {
                    slots[current].down = event.value != -1;
                } else if (event.code == ABS_MT_POSITION_X) {
                    slots[current].x = (float) event.value;
                } else if (event.code == ABS_MT_POSITION_Y) {
                    slots[current].y = (float) event.value;
                }
            } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
                std::lock_guard<std::mutex> lock(g_finger_mutex);
                g_finger_count = 0;
                for (const Slot &slot: slots) {
                    if (!slot.down) continue;
                    to_display(in, slot.x, slot.y, g_fingers[g_finger_count].x, g_fingers[g_finger_count].y);
                    ++g_finger_count;
                }
            }
        }
    };

    void input_thread(const input &_input) {
        sleep(1);
        if (!imgui_io)
            imgui_io = &ImGui::GetIO();
        input_event events[64]{0};
        SlotTracker tracker;
        while (_input.fd > 0 && imgui_io && imgui_io->BackendRendererUserData) {
            auto event_readed_count = read(_input.fd, events, sizeof(events));
            if (event_readed_count == -1)
                continue;
            event_readed_count /= (ssize_t) sizeof(input_event);

            static bool isDown = false;
            static float x = 0.0f, y = 0.0f;
            for (long j = 0; j < event_readed_count; j++) {
                auto &event = events[j];
                tracker.feed(_input, event);
                if (event.type == EV_ABS) {
#if TOUCH_MODE == 2
                    if (_input.trackingIDPresent && event.code == ABS_MT_TRACKING_ID) {
                        isDown = event.value != -1;
                        continue;
                    }
#elif TOUCH_MODE == 0
                    if (event.code == ABS_MT_TRACKING_ID) {
                        isDown = event.value != -1;
                        continue;
                    }
#endif
                    if (event.code == ABS_MT_POSITION_X) {
                        x = (float) event.value;
                        continue;
                    }
                    if (event.code == ABS_MT_POSITION_Y) {
                        y = (float) event.value;
                        continue;
                    }
                }
                if (event.code == SYN_REPORT && imgui_io) {
#if TOUCH_MODE == 2
                    imgui_io->MouseDown[0] = _input.trackingIDPresent ? isDown : event_readed_count > 2;
#elif TOUCH_MODE == 0
                    imgui_io->MouseDown[0] = isDown;
#else
                    imgui_io->MouseDown[0] = event_readed_count > 2;
#endif
                    if (imgui_io->MouseDown[0]) {
                        switch (orientation) {
                            case 1:
                                imgui_io->MousePos.x = y;
                                imgui_io->MousePos.y = _input.absX.maximum - x;
                                break;
                            case 2:
                                imgui_io->MousePos.x = _input.absX.maximum - x;
                                imgui_io->MousePos.y = _input.absY.maximum - y;
                                break;
                            case 3:
                                imgui_io->MousePos.x = _input.absY.maximum - y;
                                imgui_io->MousePos.y = x;
                                break;
                            default:
                                imgui_io->MousePos.x = x;
                                imgui_io->MousePos.y = y;
                                break;
                        }
                        imgui_io->MousePos.x *= _input.absXMultiplier;
                        imgui_io->MousePos.y *= _input.absYMultiplier;
                    }
                }
            }
        }
    }

    bool init(int32_t _screen_w, int32_t _screen_h, uint8_t _orientation) {
        orientation = _orientation;

        bool result = false;

        const char *path = "/dev/input/";
        auto dir = opendir(path);
        dirent *ptr;
        while ((ptr = readdir(dir)))
            if (strstr(ptr->d_name, "event")) {
                char event_path[128];
                sprintf(event_path, "%s%s", path, ptr->d_name);
                input _input(event_path, _screen_w, _screen_h);
                if (_input.canBeUsed) {
                    result = true;
                    inputs.push_back(_input);
                    std::thread(input_thread, _input).detach();
                }
            }
        closedir(dir);

        return result;
    }

    void update(int32_t _screen_w, int32_t _screen_h, uint8_t _orientation) {
        orientation = _orientation;

        for (auto &_input: inputs) {
            _input.calculateMultipliers(_screen_w, _screen_h);
        }
    }

    void updateOrientation(uint8_t _orientation) {
        orientation = _orientation;
    }


    int fingers(Finger *out, int max) {
        std::lock_guard<std::mutex> lock(g_finger_mutex);
        const int n = g_finger_count < max ? g_finger_count : max;
        for (int i = 0; i < n; ++i) out[i] = g_fingers[i];
        return n;
    }

    void shutdown() {
        for (auto &_input: inputs)
            _input.close();
        inputs.clear();
        imgui_io = nullptr;
    }
}
