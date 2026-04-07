/*
 * DIY VR Controller - SteamVR OpenVR Driver (v3.1 - fixed)
 * 
 * Düzeltmeler:
 *   - GetPose() icinde GetRawTrackedDevicePoses KALDIRILDI (deadlock!)
 *   - readLine timeout'unda port KAPATILMIYOR (eski calisan yaklasim)
 *   - ReadTotalTimeoutConstant arttirildi (100 → 500ms)
 *   - Gercek hata (ReadFile FALSE) ile timeout (0 byte) ayrimi
 *   - COM port auto-detect ($VR, imzasi aranir)
 *   - DTR/RTS koruması (Arduino reset onleme)
 *   - Thread-safe veri paylasimi (mutex)
 *
 * Debug log: C:\Users\emre_\diyvr_debug.txt
 */

#include <openvr_driver.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <chrono>

using namespace vr;

// ============ DEBUG LOG ============
static FILE* g_logFile = nullptr;
static std::mutex g_logMutex;

void logMsg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) {
        g_logFile = fopen("C:\\Users\\emre_\\diyvr_debug.txt", "a");
    }
    if (g_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
    }
}

// ============ SERIAL PORT ============
class SerialPort {
public:
    HANDLE hSerial = INVALID_HANDLE_VALUE;

    bool isOpen() const { return hSerial != INVALID_HANDLE_VALUE; }

    bool open(const char* port, int baud) {
        char fullPath[64];
        snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", port);
        logMsg("Serial aciliyor: %s (baud=%d)", fullPath, baud);

        hSerial = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
        if (hSerial == INVALID_HANDLE_VALUE) {
            logMsg("Serial HATA! CreateFile error=%lu", GetLastError());
            return false;
        }

        // DTR/RTS LOW — Arduino/Wemos reset onleme
        EscapeCommFunction(hSerial, CLRDTR);
        EscapeCommFunction(hSerial, CLRRTS);
        Sleep(50);

        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        GetCommState(hSerial, &dcb);
        dcb.BaudRate    = baud;
        dcb.ByteSize    = 8;
        dcb.Parity      = NOPARITY;
        dcb.StopBits    = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        if (!SetCommState(hSerial, &dcb)) {
            logMsg("SetCommState HATA! error=%lu", GetLastError());
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
            return false;
        }

        EscapeCommFunction(hSerial, CLRDTR);
        EscapeCommFunction(hSerial, CLRRTS);

        // Timeout: 500ms — Wemos'a yeterli zaman ver
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout         = 50;
        timeouts.ReadTotalTimeoutMultiplier  = 0;
        timeouts.ReadTotalTimeoutConstant    = 500;
        SetCommTimeouts(hSerial, &timeouts);

        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        logMsg("Serial OK acildi! (DTR/RTS LOW, timeout=500ms)");
        return true;
    }

    // readLine donusleri:
    //   >0  : basarili, satir okundu
    //    0  : timeout, hic byte gelmedi (normal, port ACIK KALMALI)
    //   -1  : gercek hata, port bozulmus olabilir
    int readLine(char* outBuf, int maxLen) {
        int idx = 0;
        while (idx < maxLen - 1) {
            char c;
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(hSerial, &c, 1, &bytesRead, NULL);

            if (!ok) {
                // ReadFile gercekten basarisiz oldu — port hatasi
                return -1;
            }
            if (bytesRead == 0) {
                // Timeout — hic byte gelmedi, bu normal
                // Eger hic bir sey okumadiysak 0 don
                // Eger partial okuduysak da 0 don (eksik satir at)
                return 0;
            }

            if (c == '\n') {
                outBuf[idx] = '\0';
                return idx;
            }
            if (c != '\r')
                outBuf[idx++] = c;
        }
        outBuf[idx] = '\0';
        return idx;
    }

    void close() {
        if (hSerial != INVALID_HANDLE_VALUE) {
            EscapeCommFunction(hSerial, CLRDTR);
            EscapeCommFunction(hSerial, CLRRTS);
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
        }
    }
};

// ============ COM PORT AUTO-DETECT ============
static bool probePort(const char* port, int baud) {
    char fullPath[64];
    snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", port);

    HANDLE h = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    EscapeCommFunction(h, CLRDTR);
    EscapeCommFunction(h, CLRRTS);

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(h, &dcb);
    dcb.BaudRate    = baud;
    dcb.ByteSize    = 8;
    dcb.Parity      = NOPARITY;
    dcb.StopBits    = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    SetCommState(h, &dcb);

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout        = 50;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant   = 500;
    SetCommTimeouts(h, &timeouts);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    // 3 saniye boyunca "$VR," iceren satir ara
    bool found = false;
    char buf[512];
    int idx = 0;
    DWORD startTick = GetTickCount();

    while (GetTickCount() - startTick < 3000) {
        char c;
        DWORD bytesRead = 0;
        if (!ReadFile(h, &c, 1, &bytesRead, NULL) || bytesRead == 0)
            continue;
        if (c == '\n') {
            buf[idx] = '\0';
            if (strncmp(buf, "$VR,", 4) == 0) {
                found = true;
                break;
            }
            idx = 0;
        } else if (c != '\r' && idx < (int)sizeof(buf) - 1) {
            buf[idx++] = c;
        }
    }

    EscapeCommFunction(h, CLRDTR);
    EscapeCommFunction(h, CLRRTS);
    CloseHandle(h);
    return found;
}

static std::string autoDetectPort(int baud) {
    logMsg("COM port otomatik algilama basliyor...");

    // Oncelikli: COM9
    logMsg("  Deneniyor: COM9 (varsayilan)...");
    if (probePort("COM9", baud)) {
        logMsg("  COM9 bulundu! ($VR, imzasi algilandi)");
        return "COM9";
    }

    for (int i = 3; i <= 20; i++) {
        if (i == 9) continue;
        char port[16];
        snprintf(port, sizeof(port), "COM%d", i);
        logMsg("  Deneniyor: %s...", port);
        if (probePort(port, baud)) {
            logMsg("  %s bulundu! ($VR, imzasi algilandi)", port);
            return port;
        }
    }

    logMsg("  Hicbir portta VR alicisi bulunamadi!");
    return "";
}

// ============ CONTROLLER STATE ============
struct ControllerState {
    float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
    float joyX = 0.0f, joyY = 0.0f;
    float trigger = 0.0f;
    uint8_t buttons = 0;
};

// ============ KONTROLCU CIHAZI ============
class CDIYController : public ITrackedDeviceServerDriver {
public:
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t m_props = k_ulInvalidPropertyContainer;

    VRInputComponentHandle_t m_hJoyX     = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hJoyY     = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hTrigger  = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hJoyClick = k_ulInvalidInputComponentHandle;

    std::mutex m_stateMutex;
    ControllerState m_state;

    SerialPort m_serial;
    std::thread m_serialThread;
    std::atomic<bool> m_running{false};

    std::string m_comPort;
    int m_baudRate;
    float m_offsetX, m_offsetY, m_offsetZ;
    bool m_autoDetect;

    CDIYController(const std::string& comPort, int baud,
                   float ox, float oy, float oz, bool autoDetect)
        : m_comPort(comPort), m_baudRate(baud),
          m_offsetX(ox), m_offsetY(oy), m_offsetZ(oz),
          m_autoDetect(autoDetect)
    {
        logMsg("Controller olusturuldu: port=%s baud=%d offset=(%.2f, %.2f, %.2f) autoDetect=%d",
               comPort.c_str(), baud, ox, oy, oz, autoDetect);
    }

    EVRInitError Activate(uint32_t unObjectId) override {
        m_id = unObjectId;
        m_props = VRProperties()->TrackedDeviceToPropertyContainer(m_id);
        logMsg("Activate: id=%u", unObjectId);

        VRProperties()->SetStringProperty(m_props, Prop_ModelNumber_String,     "DIYVR_Right");
        VRProperties()->SetStringProperty(m_props, Prop_SerialNumber_String,    "DIYVR-R-001");
        VRProperties()->SetInt32Property(m_props, Prop_ControllerRoleHint_Int32,
                                         TrackedControllerRole_RightHand);
        VRProperties()->SetStringProperty(m_props, Prop_ControllerType_String,  "diyvr_controller");
        VRProperties()->SetStringProperty(m_props, Prop_InputProfilePath_String,
                                          "{diyvr}/input/controller_profile.json");
        VRProperties()->SetStringProperty(m_props, Prop_RenderModelName_String,
                                          "vr_controller_vive_1_5");

        IVRDriverInput* input = VRDriverInput();
        input->CreateScalarComponent(m_props, "/input/joystick/x",
            &m_hJoyX, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        input->CreateScalarComponent(m_props, "/input/joystick/y",
            &m_hJoyY, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        input->CreateScalarComponent(m_props, "/input/trigger/value",
            &m_hTrigger, VRScalarType_Absolute, VRScalarUnits_NormalizedOneSided);
        input->CreateBooleanComponent(m_props, "/input/joystick/click", &m_hJoyClick);

        m_running = true;
        m_serialThread = std::thread(&CDIYController::serialLoop, this);
        logMsg("Activate tamamlandi, serial thread baslatildi");
        return VRInitError_None;
    }

    void Deactivate() override {
        logMsg("Deactivate");
        m_running = false;
        if (m_serialThread.joinable()) m_serialThread.join();
        m_serial.close();
        m_id = k_unTrackedDeviceIndexInvalid;
    }

    void EnterStandby() override {}
    void* GetComponent(const char* pchComponentNameAndVersion) override { return nullptr; }
    void DebugRequest(const char* pchRequest, char* pchResponseBuffer,
                      uint32_t unResponseBufferSize) override {
        if (unResponseBufferSize > 0) pchResponseBuffer[0] = '\0';
    }

    // ---- POSE (sabit offset — eski calisan yaklasim) ----

    DriverPose_t GetPose() override {
        DriverPose_t pose = {};
        pose.poseIsValid       = true;
        pose.result            = TrackingResult_Running_OK;
        pose.deviceIsConnected = true;

        pose.vecPosition[0] = m_offsetX;
        pose.vecPosition[1] = m_offsetY;
        pose.vecPosition[2] = m_offsetZ;

        ControllerState st;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            st = m_state;
        }

        pose.qRotation.w =  st.qw;
        pose.qRotation.x = -st.qx;
        pose.qRotation.y =  st.qz;
        pose.qRotation.z =  st.qy;

        pose.qWorldFromDriverRotation.w = 1;
        pose.qWorldFromDriverRotation.x = 0;
        pose.qWorldFromDriverRotation.y = 0;
        pose.qWorldFromDriverRotation.z = 0;

        pose.qDriverFromHeadRotation.w = 1;
        pose.qDriverFromHeadRotation.x = 0;
        pose.qDriverFromHeadRotation.y = 0;
        pose.qDriverFromHeadRotation.z = 0;

        return pose;
    }

    // ---- Serial Haberlesme ----
    // KRITIK: Eski calisan driver gibi — readLine 0 donerse (timeout)
    // portu KAPATMA, sadece continue yap. Portu sadece gercek hatada kapat.

    void serialLoop() {
        logMsg("Serial thread basladi");

        // SteamVR'nin ayaga kalkmasini bekle
        Sleep(3000);

        // Auto-detect
        if (m_autoDetect) {
            std::string detected = autoDetectPort(m_baudRate);
            if (!detected.empty()) {
                m_comPort = detected;
                logMsg("Auto-detect sonucu: %s", m_comPort.c_str());
            } else {
                logMsg("Auto-detect basarisiz, varsayilan: %s", m_comPort.c_str());
            }
        }

        // Portu ac
        logMsg("Serial aciliyor: %s", m_comPort.c_str());
        if (!m_serial.open(m_comPort.c_str(), m_baudRate)) {
            logMsg("Serial acilamadi! Thread sonlaniyor.");
            return;
        }

        // Wemos'un stabilize olmasini bekle
        Sleep(2000);
        PurgeComm(m_serial.hSerial, PURGE_RXCLEAR);
        logMsg("Serial hazir, veri okunuyor...");

        char line[256];
        int lineCount = 0;
        int consecutiveErrors = 0;

        while (m_running) {
            int len = m_serial.readLine(line, sizeof(line));

            // === TIMEOUT (len == 0): Veri yok ama port saglikli ===
            // Portu KAPATMA, sadece bekle ve tekrar dene
            if (len == 0) {
                consecutiveErrors = 0;
                continue;
            }

            // === GERCEK HATA (len == -1): ReadFile basarisiz ===
            if (len < 0) {
                consecutiveErrors++;
                if (consecutiveErrors >= 10) {
                    logMsg("10 ardisik okuma hatasi, port kopmus olabilir.");
                    logMsg("Yeniden baglanti deneniyor...");
                    m_serial.close();
                    Sleep(3000);

                    if (!m_serial.open(m_comPort.c_str(), m_baudRate)) {
                        logMsg("Yeniden baglanti basarisiz, thread sonlaniyor.");
                        return;
                    }
                    Sleep(2000);
                    PurgeComm(m_serial.hSerial, PURGE_RXCLEAR);
                    logMsg("Yeniden baglandi, veri okunuyor...");
                    consecutiveErrors = 0;
                }
                continue;
            }

            // === BASARILI OKUMA (len > 0) ===
            consecutiveErrors = 0;

            if (lineCount < 5) {
                logMsg("Satir[%d]: %s", lineCount, line);
            }
            lineCount++;

            // Protokol dogrulama
            if (strncmp(line, "$VR,", 4) != 0) continue;

            // TIMEOUT ve LOST satirlarini atla
            if (strncmp(line + 4, "TIMEOUT", 7) == 0 ||
                strncmp(line + 4, "LOST", 4) == 0) continue;

            float tqw, tqx, tqy, tqz;
            int tjx, tjy, ttrig, tbtn;

            int parsed = sscanf(line + 4, "%f,%f,%f,%f,%d,%d,%d,%d",
                                &tqw, &tqx, &tqy, &tqz,
                                &tjx, &tjy, &ttrig, &tbtn);
            if (parsed != 8) {
                if (lineCount < 20)
                    logMsg("Parse hatasi! parsed=%d line=%s", parsed, line);
                continue;
            }

            // Quaternion normalize
            float norm = sqrtf(tqw*tqw + tqx*tqx + tqy*tqy + tqz*tqz);
            if (norm > 0.001f) {
                tqw /= norm; tqx /= norm; tqy /= norm; tqz /= norm;
            }

            // Thread-safe guncelle
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_state.qw = tqw;
                m_state.qx = tqx;
                m_state.qy = tqy;
                m_state.qz = tqz;
                m_state.joyX    = clampf(tjx / 512.0f, -1.0f, 1.0f);
                m_state.joyY    = clampf(tjy / 512.0f, -1.0f, 1.0f);
                m_state.trigger = clampf(ttrig / 1000.0f, 0.0f, 1.0f);
                m_state.buttons = (uint8_t)tbtn;
            }

            // Pose guncelle
            if (m_id != k_unTrackedDeviceIndexInvalid) {
                VRServerDriverHost()->TrackedDevicePoseUpdated(
                    m_id, GetPose(), sizeof(DriverPose_t));
            }

            // Input guncelle
            IVRDriverInput* input = VRDriverInput();
            ControllerState snap;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                snap = m_state;
            }
            double now = nowSeconds();
            input->UpdateScalarComponent(m_hJoyX,     snap.joyX,    now);
            input->UpdateScalarComponent(m_hJoyY,     snap.joyY,    now);
            input->UpdateScalarComponent(m_hTrigger,  snap.trigger, now);
            input->UpdateBooleanComponent(m_hJoyClick, (snap.buttons & 0x01) != 0, now);

            if (lineCount == 10) {
                logMsg("10 satir basariyla okundu, calisiyor!");
            }
        }
        logMsg("Serial thread sonlandi. Toplam %d satir.", lineCount);
    }

private:
    static float clampf(float v, float lo, float hi) {
        return (v < lo) ? lo : (v > hi) ? hi : v;
    }

    static double nowSeconds() {
        using namespace std::chrono;
        auto t = steady_clock::now().time_since_epoch();
        return duration_cast<duration<double>>(t).count();
    }
};

// ============ SURUCU SAGLAYICI ============
class CServerProvider : public IServerTrackedDeviceProvider {
public:
    CDIYController* m_controller = nullptr;

    EVRInitError Init(IVRDriverContext* pDriverContext) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        logMsg("=== DIYVR Driver Init (v3.1) ===");

        // Varsayilan ayarlar
        std::string comPort = "COM9";
        int baud = 115200;
        float ox = 0.2f, oy = -0.3f, oz = -0.25f;
        bool autoDetect = true;

        // vrsettings'ten oku
        IVRSettings* settings = VRSettings();
        if (settings) {
            const char* section = "driver_diyvr";
            EVRSettingsError err;

            char portBuf[64] = {};
            settings->GetString(section, "serialPort", portBuf, sizeof(portBuf), &err);
            if (err == VRSettingsError_None && portBuf[0] != '\0') {
                comPort = portBuf;
                autoDetect = false;
                logMsg("vrsettings'ten port okundu: %s", comPort.c_str());
            }

            int b = settings->GetInt32(section, "baudRate", &err);
            if (err == VRSettingsError_None && b > 0) baud = b;

            float v;
            v = settings->GetFloat(section, "controllerOffsetX", &err);
            if (err == VRSettingsError_None) ox = v;
            v = settings->GetFloat(section, "controllerOffsetY", &err);
            if (err == VRSettingsError_None) oy = v;
            v = settings->GetFloat(section, "controllerOffsetZ", &err);
            if (err == VRSettingsError_None) oz = v;
        }

        logMsg("Ayarlar: port=%s baud=%d offset=(%.2f, %.2f, %.2f) autoDetect=%d",
               comPort.c_str(), baud, ox, oy, oz, autoDetect);

        m_controller = new CDIYController(comPort, baud, ox, oy, oz, autoDetect);
        VRServerDriverHost()->TrackedDeviceAdded(
            "DIYVR-R-001", TrackedDeviceClass_Controller, m_controller);

        logMsg("TrackedDeviceAdded cagirildi");
        return VRInitError_None;
    }

    void Cleanup() override {
        logMsg("Cleanup");
        if (m_controller) {
            delete m_controller;
            m_controller = nullptr;
        }
        if (g_logFile) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    const char* const* GetInterfaceVersions() override {
        return k_InterfaceVersions;
    }

    void RunFrame() override {}
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}
};

static CServerProvider g_serverProvider;

extern "C" __declspec(dllexport)
void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode) {
    if (strcmp(pInterfaceName, IServerTrackedDeviceProvider_Version) == 0) {
        return &g_serverProvider;
    }
    if (pReturnCode) *pReturnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
