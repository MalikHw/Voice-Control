#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <thread>
#include <atomic>
#include <cmath>
#include <chrono>
#include <vector>
#include <string>
#include <utility>

#ifndef GEODE_IS_ANDROID
#include <fmod.hpp>
#endif

#ifdef GEODE_IS_ANDROID
#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#define ANDROID_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "VoiceControl", __VA_ARGS__)

static JavaVM* getJavaVM() {
    using GetCreatedJavaVMs_t = jint(*)(JavaVM**, jsize, jsize*);

    void* libart = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libart) libart = dlopen("libdvm.so", RTLD_NOW | RTLD_NOLOAD);
    if (!libart) return nullptr;

    auto fn = (GetCreatedJavaVMs_t)dlsym(libart, "JNI_GetCreatedJavaVMs");
    if (!fn) return nullptr;

    JavaVM* vm = nullptr;
    jsize count = 0;
    fn(&vm, 1, &count);
    return (count > 0) ? vm : nullptr;
}
#endif

using namespace geode::prelude;

// hi
static std::atomic<bool>  g_running(false);
static std::atomic<bool>  g_threadReady(false);
static std::atomic<bool>  g_wasAbove(false); // shared so PlayLayer hooks can reset it
static std::atomic<int>   g_selectedDevice(-1);
static std::atomic<int>   g_activeDevice(0);
static std::atomic<bool>  g_restartRequested(false);
static std::atomic<float> g_currentPeak(0.0f);
static std::thread        g_micThread;

#ifndef GEODE_IS_ANDROID
static FMOD::System* g_fmodSystem = nullptr;
static FMOD::Sound*  g_recordSound = nullptr;
static int           g_recordDevice = 0;
static const int RECORD_LEN = 1024 * 2;
#endif

// ok uhm i saw a lot of tiktoks of someone making an external program for this so i made it for geode .-. lol
// axiom was here
// mic detection is literally just comparing db levels, if peak is above threshold = jump. thats it
// https://www.youtube.com/shorts/ZjJJFN9lXuY :O

static bool canClick() {
    PlayLayer* pl = PlayLayer::get();
    return pl && !pl->m_isPaused && Mod::get()->getSettingValue<bool>("enabled");
}

static float get_threshold() {
    return (float)Mod::get()->getSettingValue<double>("threshold-db");
}

#ifndef GEODE_IS_ANDROID

struct MicInfo {
    int index;
    std::string name;
};

static bool is_speaker(const std::string& name) { // for the ppl who for some reason have 12 speakrers plugged in 
    std::string lower = name;
    for (char& c : lower) c = tolower(c);
    if (lower.find("speaker") != std::string::npos) return true;
    if (lower.find("headphone") != std::string::npos) return true;
    if (lower.find("headset") != std::string::npos) return true;
    if (lower.find("output") != std::string::npos) return true;
    if (lower.find("speakers") != std::string::npos) return true;
    if (lower.find("audio output") != std::string::npos) return true;
    if (lower.find("realtek audio") != std::string::npos) return true;
    return false;
}

static std::vector<MicInfo> list_mics() {
    std::vector<MicInfo> out;
    if (!g_fmodSystem) return out;
    int num = 0, conn = 0;
    g_fmodSystem->getRecordNumDrivers(&num, &conn);
    for (int i = 0; i < num; i++) {
        char name[256] = {0};
        FMOD_RESULT r = g_fmodSystem->getRecordDriverInfo(
            i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr, nullptr
        );
        if (r != FMOD_OK) continue;
        std::string sname(name);
        if (is_speaker(sname)) continue;
        out.push_back({ i, sname });
    }
    return out;
}

static int find_mic_with_audio() {
    // fmod is stupid so we gotta do this manually
    int numDevices = 0, numConnected = 0;
    g_fmodSystem->getRecordNumDrivers(&numDevices, &numConnected);

    for (int i = 0; i < numDevices; i++) {
        char name[256] = {0};
        g_fmodSystem->getRecordDriverInfo(i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (is_speaker(std::string(name))) continue;

        FMOD_CREATESOUNDEXINFO ex;
        memset(&ex, 0, sizeof(ex));
        ex.cbsize = sizeof(ex);
        ex.numchannels = 1;
        ex.format = FMOD_SOUND_FORMAT_PCM16;
        ex.defaultfrequency = 44100;
        ex.length = RECORD_LEN * sizeof(short);

        FMOD::Sound* test = nullptr;
        if (g_fmodSystem->createSound(nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &ex, &test) != FMOD_OK)
            continue;
        if (g_fmodSystem->recordStart(i, test, true) != FMOD_OK) {
            test->release();
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        unsigned int curr = 0;
        g_fmodSystem->getRecordPosition(i, &curr);
        g_fmodSystem->recordStop(i);
        test->release();

        if (curr > 0) return i;
    }
    return 0;// fallbackkkkkkkkkkkkkkkkkkk
}

#endif // !GEODE_IS_ANDROID (MicInfo / is_speaker / list_mics / find_mic_with_audio)

#ifndef GEODE_IS_ANDROID
static void mic_thread_func() {
    FMOD::System_Create(&g_fmodSystem);
    g_fmodSystem->init(1, FMOD_INIT_NORMAL, nullptr);

    int saved = Mod::get()->getSavedValue<int>("selected-device", -1);
    g_selectedDevice.store(saved);

    if (saved >= 0) {
        g_recordDevice = saved;
    } else {
        g_recordDevice = find_mic_with_audio();
    }

    FMOD_CREATESOUNDEXINFO exparams;
    memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
    exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
    exparams.numchannels = 1; // mono only, stereo was overkill
    exparams.format = FMOD_SOUND_FORMAT_PCM16;
    exparams.defaultfrequency = 44100; // tried 48k, didnt matter
    exparams.length = RECORD_LEN * sizeof(short);

    g_fmodSystem->createSound(nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &g_recordSound);
    g_fmodSystem->recordStart(g_recordDevice, g_recordSound, true);
    g_activeDevice.store(g_recordDevice);

    g_threadReady.store(true);

    unsigned int pos = 0;
    int release_counter = 0;
    const int RELEASE_FRAMES = 1;

    while (g_running.load()) {
        g_fmodSystem->update();

        if (g_restartRequested.exchange(false)) {
            g_fmodSystem->recordStop(g_recordDevice);
            if (g_recordSound) {
                g_recordSound->release();
                g_recordSound = nullptr;
            }

            int newdev = g_selectedDevice.load();
            if (newdev < 0) newdev = find_mic_with_audio();
            g_recordDevice = newdev;

            memset(&exparams, 0, sizeof(FMOD_CREATESOUNDEXINFO));
            exparams.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
            exparams.numchannels = 1;
            exparams.format = FMOD_SOUND_FORMAT_PCM16;
            exparams.defaultfrequency = 44100;
            exparams.length = RECORD_LEN * sizeof(short);

            g_fmodSystem->createSound(nullptr, FMOD_2D | FMOD_OPENUSER | FMOD_LOOP_NORMAL, &exparams, &g_recordSound);
            g_fmodSystem->recordStart(g_recordDevice, g_recordSound, true);
            g_activeDevice.store(g_recordDevice);
            pos = 0;
            g_currentPeak.store(0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        unsigned int curr;
        g_fmodSystem->getRecordPosition(g_recordDevice, &curr);

        if (curr == pos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        int samples = (static_cast<int>(curr) - static_cast<int>(pos) + RECORD_LEN) % RECORD_LEN;
        bool was_above = g_wasAbove.load();

        if (samples > 0) {
            void* p1 = nullptr; void* p2 = nullptr;
            unsigned int l1 = 0; unsigned int l2 = 0;

            g_recordSound->lock(pos * sizeof(short), samples * sizeof(short), &p1, &p2, &l1, &l2);

            float peak = 0.0f;
            float sumsq = 0.0f;
            int cnt = 0;

            auto process = [&](void* p, unsigned int l) {
                if (!p) return;
                short* buf = (short*)p;
                int n = l / sizeof(short);
                for (int i = 0; i < n; i++) {
                    float s = buf[i] / 32768.0f;
                    float abss = s < 0.0f ? -s : s;
                    if (abss > peak) peak = abss;
                    sumsq += s * s;
                    cnt++;
                }
            };
            process(p1, l1);
            process(p2, l2);

            g_recordSound->unlock(p1, p2, l1, l2);

            float peak_db = (peak <= 0.0f) ? -100.0f : 20.0f * log10f(peak);
            float rms     = (cnt > 0) ? sqrtf(sumsq / (float)cnt) : 0.0f;
            float rms_db  = (rms <= 0.0f) ? -100.0f : 20.0f * log10f(rms);

            float prev = g_currentPeak.load();
            float disp = (peak > prev) ? peak : (prev * 0.85f + peak * 0.15f);
            g_currentPeak.store(disp);

            float thresh         = get_threshold();
            float release_thresh = thresh - 12.0f;

            bool peak_above = peak_db >= thresh;
            bool rms_above  = rms_db  >= release_thresh;

            if (peak_above && !was_above) {
                g_wasAbove.store(true);
                release_counter = 0;
                Loader::get()->queueInMainThread([]() {
                    if (canClick()) {
                        PlayLayer::get()->handleButton(true, (int)PlayerButton::Jump, true);
                    } else {
                        g_wasAbove.store(false);
                    }
                });
            // still loud, keep holding
            } else if (was_above && !peak_above && !rms_above) {
                release_counter++;
                if (release_counter >= RELEASE_FRAMES) {
                    g_wasAbove.store(false);
                    release_counter = 0;
                    Loader::get()->queueInMainThread([]() {
                        PlayLayer* pl = PlayLayer::get();
                        if (pl && !pl->m_isPaused && pl->m_player1)
                            pl->handleButton(false, (int)PlayerButton::Jump, true);
                    });
                }

            } else if (peak_above && was_above) {
                release_counter = 0;
            }
        }

        pos = curr;
        // -sleep so were not burning cpu. 2ms seems like a good balance, talking from experience-, nvm going to 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    g_fmodSystem->recordStop(g_recordDevice);
    if (g_recordSound) {
        g_recordSound->release();
        g_recordSound = nullptr;
    }
    g_fmodSystem->release();
    g_fmodSystem = nullptr;
}

#endif // !GEODE_IS_ANDROID

#ifdef GEODE_IS_ANDROID

static bool getJNIEnv(JavaVM* vm, JNIEnv** env) {
    int status = vm->GetEnv((void**)env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(env, nullptr) != 0) {
            *env = nullptr;
            return false;
        }
        return true;
    }
    return false;
}

static void mic_thread_func() {
    JavaVM* vm = getJavaVM();
    if (!vm) {
        ANDROID_LOG("mic_thread_func: no JavaVM!");
        return;
    }

    JNIEnv* env = nullptr;
    bool didAttach = getJNIEnv(vm, &env);
    if (!env) {
        ANDROID_LOG("mic_thread_func: failed to get JNIEnv");
        return;
    }

    const int MIC_SOURCE        = 1;
    const int CHANNEL_IN_MONO   = 16;
    const int ENCODING_PCM_16   = 2;
    const int SAMPLE_RATE       = 44100;
    const int STATE_INITIALIZED = 1;
    const int RECORDSTATE_RECORDING = 3;

    jclass arClass = env->FindClass("android/media/AudioRecord");
    if (!arClass) {
        ANDROID_LOG("mic_thread_func: AudioRecord class not found");
        if (didAttach) vm->DetachCurrentThread();
        return;
    }

    jmethodID getMinBufSizeMethod = env->GetStaticMethodID(arClass, "getMinBufferSize", "(III)I");
    int minBuf = env->CallStaticIntMethod(arClass, getMinBufSizeMethod,
        SAMPLE_RATE, CHANNEL_IN_MONO, ENCODING_PCM_16);
    if (minBuf <= 0) minBuf = 4096;

    int bufferSize = minBuf * 2;

    jmethodID ctorMethod = env->GetMethodID(arClass, "<init>", "(IIIII)V");
    jobject audioRecord = env->NewObject(arClass, ctorMethod,
        MIC_SOURCE, SAMPLE_RATE, CHANNEL_IN_MONO, ENCODING_PCM_16, bufferSize);
    if (!audioRecord) {
        ANDROID_LOG("mic_thread_func: failed to create AudioRecord");
        if (didAttach) vm->DetachCurrentThread();
        return;
    }

    jmethodID getStateMethod = env->GetMethodID(arClass, "getState", "()I");
    int state = env->CallIntMethod(audioRecord, getStateMethod);
    if (state != STATE_INITIALIZED) {
        ANDROID_LOG("mic_thread_func: AudioRecord not initialized (state=%d)", state);
        jmethodID releaseMethod = env->GetMethodID(arClass, "release", "()V");
        env->CallVoidMethod(audioRecord, releaseMethod);
        if (didAttach) vm->DetachCurrentThread();
        return;
    }

    jmethodID startRecordingMethod = env->GetMethodID(arClass, "startRecording", "()V");
    jmethodID readMethod = env->GetMethodID(arClass, "read", "([SII)I");
    jmethodID stopMethod = env->GetMethodID(arClass, "stop", "()V");
    jmethodID releaseMethod = env->GetMethodID(arClass, "release", "()V");

    env->CallVoidMethod(audioRecord, startRecordingMethod);
    ANDROID_LOG("mic_thread_func: AudioRecord started, bufferSize=%d", bufferSize);

    g_threadReady.store(true);

    const int READ_SAMPLES = bufferSize / 2;
    jshortArray jbuf = env->NewShortArray(READ_SAMPLES);

    int release_counter = 0;
    const int RELEASE_FRAMES = 1;

    while (g_running.load()) {
        jint read = env->CallIntMethod(audioRecord, readMethod, jbuf, 0, READ_SAMPLES);
        if (read <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        jshort* buf = env->GetShortArrayElements(jbuf, nullptr);

        float peak = 0.0f;
        float sumsq = 0.0f;
        for (int i = 0; i < read; i++) {
            float s = buf[i] / 32768.0f;
            float abss = s < 0.0f ? -s : s;
            if (abss > peak) peak = abss;
            sumsq += s * s;
        }

        env->ReleaseShortArrayElements(jbuf, buf, JNI_ABORT);

        float rms    = sqrtf(sumsq / (float)read);
        float peak_db = (peak <= 0.0f) ? -100.0f : 20.0f * log10f(peak);
        float rms_db  = (rms  <= 0.0f) ? -100.0f : 20.0f * log10f(rms);

        float prev = g_currentPeak.load();
        float disp = (peak > prev) ? peak : (prev * 0.85f + peak * 0.15f);
        g_currentPeak.store(disp);

        float thresh         = get_threshold();
        float release_thresh = thresh - 12.0f;
        bool  was_above      = g_wasAbove.load();
        bool  peak_above     = peak_db >= thresh;
        bool  rms_above      = rms_db  >= release_thresh;

        if (peak_above && !was_above) {
            g_wasAbove.store(true);
            release_counter = 0;
            Loader::get()->queueInMainThread([]() {
                if (canClick()) {
                    PlayLayer::get()->handleButton(true, (int)PlayerButton::Jump, true);
                } else {
                    g_wasAbove.store(false);
                }
            });
        } else if (was_above && !peak_above && !rms_above) {
            release_counter++;
            if (release_counter >= RELEASE_FRAMES) {
                g_wasAbove.store(false);
                release_counter = 0;
                Loader::get()->queueInMainThread([]() {
                    PlayLayer* pl = PlayLayer::get();
                    if (pl && !pl->m_isPaused && pl->m_player1)
                        pl->handleButton(false, (int)PlayerButton::Jump, true);
                });
            }
        } else if (peak_above && was_above) {
            release_counter = 0;
        }
    }

    env->DeleteLocalRef(jbuf);
    env->CallVoidMethod(audioRecord, stopMethod);
    env->CallVoidMethod(audioRecord, releaseMethod);
    env->DeleteLocalRef(audioRecord);
    env->DeleteLocalRef(arClass);

    if (didAttach) vm->DetachCurrentThread();
    ANDROID_LOG("mic_thread_func: stopped");
}

#endif // GEODE_IS_ANDROID

static void start_mic() {
    if (g_running.load()) return;
    g_running.store(true);
    g_threadReady.store(false);
    g_micThread = std::thread(mic_thread_func);
}
// reason is people cant fucking set their default mic to wtw they are using and etc
#ifndef GEODE_IS_ANDROID
class MicSelectPopup : public Popup {
public:
    std::vector<MicInfo> m_mics;
    int m_selectedIdx = -1;
    float m_blinkPhase = 0.f;
    std::vector<std::pair<int, CCLayerColor*>> m_dots;
    std::vector<std::pair<int, CCLayerColor*>> m_bars;
    std::vector<std::pair<int, CCMenuItemSpriteExtra*>> m_buttons;

    bool init(float w, float h) {
        if (!Popup::init(w, h)) return false;

        auto winSize = m_mainLayer->getContentSize();

        auto title = CCLabelBMFont::create("Select Microphone", "bigFont.fnt");
        title->setScale(0.6f);
        title->setPosition(winSize.width / 2.f, winSize.height - 18.f);
        m_mainLayer->addChild(title);

        m_mics = list_mics();
        m_selectedIdx = g_selectedDevice.load();

        auto container = CCNode::create();
        float cw = winSize.width - 30.f;
        float ch = winSize.height - 60.f;
        container->setContentSize({ cw, ch });
        container->setAnchorPoint({ 0.5f, 0.5f });
        container->setPosition(winSize.width / 2, winSize.height / 2 - 8.f);
        m_mainLayer->addChild(container);

        auto buttonsMenu = CCMenu::create();
        buttonsMenu->setPosition(0, 0);
        buttonsMenu->ignoreAnchorPointForPosition(true);
        buttonsMenu->setContentSize(container->getContentSize());
        container->addChild(buttonsMenu);

        std::vector<std::pair<int, std::string>> rows;
        rows.push_back({ -1, "Auto detect" });
        for (auto& m : m_mics) rows.push_back({ m.index, m.name });

        const float rowHeight = 28.f;
        const float topPad = 4.f;

        for (size_t i = 0; i < rows.size(); i++) {
            int idx = rows[i].first;
            const std::string& name = rows[i].second;
            float yTop = ch - topPad - rowHeight * (float)i;
            float yMid = yTop - rowHeight / 2.f;

            auto rowBg = CCLayerColor::create({ 0, 0, 0, 70 }, cw, rowHeight - 2.f);
            rowBg->setPosition(0.f, yTop - rowHeight + 1.f);
            container->addChild(rowBg);

            auto dot = CCLayerColor::create({ 0, 255, 0, 60 }, 12.f, 12.f);
            dot->setPosition(6.f, yMid - 6.f);
            container->addChild(dot);
            m_dots.push_back({ idx, dot });

            auto barBg = CCLayerColor::create({ 30, 30, 30, 120 }, 80.f, 6.f);
            barBg->setPosition(cw - 140.f, yMid - 3.f);
            container->addChild(barBg);

            auto bar = CCLayerColor::create({ 0, 220, 60, 220 }, 1.f, 6.f);
            bar->setPosition(cw - 140.f, yMid - 3.f);
            container->addChild(bar);
            m_bars.push_back({ idx, bar });

            auto lbl = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
            lbl->setAnchorPoint({ 0.f, 0.5f });
            lbl->setScale(0.45f);
            lbl->setPosition(24.f, yMid);
            lbl->limitLabelWidth(cw - 240.f, 0.45f, 0.2f);
            container->addChild(lbl);

            auto bs = ButtonSprite::create(
                (m_selectedIdx == idx) ? "Selected" : "Select",
                "bigFont.fnt", "GJ_button_05.png", 0.6f
            );
            bs->setScale(0.5f);
            auto btn = CCMenuItemSpriteExtra::create(bs, this, menu_selector(MicSelectPopup::onSelect));
            btn->setPosition(cw - 36.f, yMid);
            btn->setUserData((void*)(intptr_t)idx);
            buttonsMenu->addChild(btn);
            m_buttons.push_back({ idx, btn });
        }

        this->schedule(schedule_selector(MicSelectPopup::tick), 0.04f);
        return true;
    }

    void onSelect(CCObject* sender) {
        auto node = static_cast<CCNode*>(sender);
        int idx = (int)(intptr_t)node->getUserData();
        g_selectedDevice.store(idx);
        Mod::get()->setSavedValue<int>("selected-device", idx);
        g_restartRequested.store(true);
        m_selectedIdx = idx;

        for (auto& kv : m_buttons) {
            auto bs = static_cast<ButtonSprite*>(kv.second->getNormalImage());
            if (bs) bs->setString((kv.first == idx) ? "Selected" : "Select");
        }
    }
// here undefined0 or whoever is reviewing
    void tick(float dt) {
        int active = g_activeDevice.load();
        int sel    = g_selectedDevice.load();
        float peak = g_currentPeak.load();

        m_blinkPhase += dt;
        bool blinkOn  = (fmodf(m_blinkPhase, 0.6f) < 0.3f);
        bool hasAudio = peak > 0.02f;

        for (auto& kv : m_dots) {
            int idx = kv.first;
            auto dot = kv.second;
            bool isActive = (idx == active) || (idx == -1 && sel == -1);

            GLubyte op = 50;
            if (isActive) {
                if (hasAudio) op = blinkOn ? 255 : 110;
                else          op = 90;
            }
            dot->setOpacity(op);
        }

        for (auto& kv : m_bars) {
            int idx = kv.first;
            auto bar = kv.second;
            bool isActive = (idx == active) || (idx == -1 && sel == -1);
            float w = isActive ? std::min(1.0f, peak * 2.0f) * 80.f : 0.f;
            if (w < 1.f) w = 1.f;
            bar->setContentSize({ w, 6.f });
        }
    }

public:
    static MicSelectPopup* create() {
        auto ret = new MicSelectPopup();
        if (ret->init(380.f, 260.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};
#endif // !GEODE_IS_ANDROID

// simplest ui but we ball
class $modify(PlayLayer) {
    void resetLevel() {
        g_wasAbove.store(false);
        PlayLayer::resetLevel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        g_wasAbove.store(false);
        PlayLayer::destroyPlayer(player, object);
    }
};

class $modify(VCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

#ifndef GEODE_IS_ANDROID
        auto menu = getChildByID("right-button-menu");
        if (!menu) menu = getChildByID("left-button-menu");
        if (!menu) return;

        CCSprite* spr = CCSprite::create("logo.png"_spr);
        if (!spr) spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        spr->setScale(0.45f);

        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(VCPauseLayer::onMicSelect));
        btn->setID("voicecontrol-mic-btn"_spr);
        menu->addChild(btn);
        menu->updateLayout();
#endif
    }

#ifndef GEODE_IS_ANDROID
    void onMicSelect(CCObject*) {
        if (auto popup = MicSelectPopup::create()) popup->show();
    }
#endif
};

$execute {
    start_mic();
}