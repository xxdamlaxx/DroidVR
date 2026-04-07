/*
 * DIY VR Controller - SteamVR OpenVR Driver
 * Debug log: C:\diyvr_debug.txt
 */

#include <openvr_driver.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>

using namespace vr;

// ============ DEBUG LOG ============
static FILE* g_logFile = nullptr;

void logMsg(const char* fmt, ...) {
    if (!g_logFile) {
        g_logFile = fopen("C:\\Users\\emre_\\diyvr_debug.txt", "a");
    }
    if (g_logFile) {
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

    bool open(const char* port, int baud) {
        char fullPath[64];
        snprintf(fullPath, sizeof(fullPath), "\\\\.\\%s", port);
        logMsg("Serial aciliyor: %s (baud=%d)", fullPath, baud);

        hSerial = CreateFileA(fullPath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
        if (hSerial == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            logMsg("Serial HATA! CreateFile failed, error=%lu", err);
            return false;
        }

        DCB dcb = {};
        dcb.DCBlength = sizeof(DCB);
        GetCommState(hSerial, &dcb);
        dcb.BaudRate = baud;
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        SetCommState(hSerial, &dcb);

        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = 10;
        timeouts.ReadTotalTimeoutMultiplier = 0;
        timeouts.ReadTotalTimeoutConstant = 100;
        SetCommTimeouts(hSerial, &timeouts);

        logMsg("Serial OK acildi!");
        return true;
    }

    int readLine(char* buf, int maxLen) {
        int idx = 0;
        char c;
        DWORD bytesRead;
        while (idx < maxLen - 1) {
            if (!ReadFile(hSerial, &c, 1, &bytesRead, NULL) || bytesRead == 0)
                return -1;
            if (c == '\n') { buf[idx] = '\0'; return idx; }
            if (c != '\r') buf[idx++] = c;
        }
        buf[idx] = '\0';
        return idx;
    }

    void close() {
        if (hSerial != INVALID_HANDLE_VALUE) {
            CloseHandle(hSerial);
            hSerial = INVALID_HANDLE_VALUE;
        }
    }
};

// ============ KONTROLCU CIHAZI ============
class CDIYController : public ITrackedDeviceServerDriver {
public:
    uint32_t m_id = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t m_props = k_ulInvalidPropertyContainer;

    VRInputComponentHandle_t m_hJoyX = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hJoyY = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hTrigger = k_ulInvalidInputComponentHandle;
    VRInputComponentHandle_t m_hJoyClick = k_ulInvalidInputComponentHandle;

    float m_qw = 1, m_qx = 0, m_qy = 0, m_qz = 0;
    float m_joyX = 0, m_joyY = 0;
    float m_trigger = 0;
    uint8_t m_buttons = 0;

    SerialPort m_serial;
    std::thread m_serialThread;
    std::atomic<bool> m_running{false};

    std::string m_comPort;
    int m_baudRate;
    float m_offsetX, m_offsetY, m_offsetZ;

    CDIYController(const std::string& comPort, int baud,
                   float ox, float oy, float oz)
        : m_comPort(comPort), m_baudRate(baud),
          m_offsetX(ox), m_offsetY(oy), m_offsetZ(oz) {
        logMsg("Controller olusturuldu: port=%s baud=%d offset=%.2f,%.2f,%.2f",
               comPort.c_str(), baud, ox, oy, oz);
    }

    EVRInitError Activate(uint32_t unObjectId) override {
        m_id = unObjectId;
        m_props = VRProperties()->TrackedDeviceToPropertyContainer(m_id);
        logMsg("Activate: id=%u", unObjectId);

        VRProperties()->SetStringProperty(m_props, Prop_ModelNumber_String, "DIYVR_Right");
        VRProperties()->SetStringProperty(m_props, Prop_SerialNumber_String, "DIYVR-R-001");
        VRProperties()->SetInt32Property(m_props, Prop_ControllerRoleHint_Int32,
                                         TrackedControllerRole_RightHand);
        VRProperties()->SetStringProperty(m_props, Prop_ControllerType_String, "diyvr_controller");
        VRProperties()->SetStringProperty(m_props, Prop_InputProfilePath_String,
                                          "{diyvr}/input/controller_profile.json");
        VRProperties()->SetStringProperty(m_props, Prop_RenderModelName_String,
                                          "vr_controller_vive_1_5");

        IVRDriverInput* input = VRDriverInput();
        input->CreateScalarComponent(m_props, "/input/joystick/x",
            &m_hJoyX, EVRScalarType::VRScalarType_Absolute,
            EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
        input->CreateScalarComponent(m_props, "/input/joystick/y",
            &m_hJoyY, EVRScalarType::VRScalarType_Absolute,
            EVRScalarUnits::VRScalarUnits_NormalizedTwoSided);
        input->CreateScalarComponent(m_props, "/input/trigger/value",
            &m_hTrigger, EVRScalarType::VRScalarType_Absolute,
            EVRScalarUnits::VRScalarUnits_NormalizedOneSided);
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

    DriverPose_t GetPose() override {
        DriverPose_t pose = {};
        pose.poseIsValid = true;
        pose.result = TrackingResult_Running_OK;
        pose.deviceIsConnected = true;

        pose.vecPosition[0] = m_offsetX;
        pose.vecPosition[1] = m_offsetY;
        pose.vecPosition[2] = m_offsetZ;

        pose.qRotation.w = m_qw;
        pose.qRotation.x = -m_qx;
        pose.qRotation.y = m_qz;
        pose.qRotation.z = m_qy;

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

    void serialLoop() {
        logMsg("Serial thread basladi, port=%s", m_comPort.c_str());
        Sleep(3000);

        if (!m_serial.open(m_comPort.c_str(), m_baudRate)) {
            logMsg("Serial acilamadi! Thread sonlaniyor.");
            return;
        }

        Sleep(2000);
        logMsg("Serial hazir, veri okunuyor...");

        char line[256];
        int lineCount = 0;
        while (m_running) {
            int len = m_serial.readLine(line, sizeof(line));
            if (len <= 0) continue;

            if (lineCount < 5) {
                logMsg("Alinan satir: %s", line);
            }
            lineCount++;

            if (strncmp(line, "$VR,", 4) != 0) continue;

            float tqw, tqx, tqy, tqz;
            int tjx, tjy, ttrig, tbtn;

            int parsed = sscanf(line + 4, "%f,%f,%f,%f,%d,%d,%d,%d",
                                &tqw, &tqx, &tqy, &tqz,
                                &tjx, &tjy, &ttrig, &tbtn);
            if (parsed != 8) {
                logMsg("Parse hatasi! parsed=%d line=%s", parsed, line);
                continue;
            }

            m_qw = tqw; m_qx = tqx; m_qy = tqy; m_qz = tqz;
            m_joyX = tjx / 512.0f;
            m_joyY = tjy / 512.0f;
            m_trigger = ttrig / 1000.0f;
            m_buttons = (uint8_t)tbtn;

            if (m_id != k_unTrackedDeviceIndexInvalid) {
                VRServerDriverHost()->TrackedDevicePoseUpdated(
                    m_id, GetPose(), sizeof(DriverPose_t));
            }

            IVRDriverInput* input = VRDriverInput();
            double now = 0;
            input->UpdateScalarComponent(m_hJoyX, m_joyX, now);
            input->UpdateScalarComponent(m_hJoyY, m_joyY, now);
            input->UpdateScalarComponent(m_hTrigger, m_trigger, now);
            input->UpdateBooleanComponent(m_hJoyClick, (m_buttons & 0x01) != 0, now);

            if (lineCount == 10) {
                logMsg("10 satir basariyla okundu, calisiyor!");
            }
        }
        logMsg("Serial thread sonlandi. Toplam %d satir.", lineCount);
    }
};

// ============ SURUCU SAGLAYICI ============
class CServerProvider : public IServerTrackedDeviceProvider {
public:
    CDIYController* m_controller = nullptr;

    EVRInitError Init(IVRDriverContext* pDriverContext) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
        logMsg("=== DIYVR Driver Init ===");

        const char* comPort = "COM9";
        int baud = 115200;
        float ox = 0.0f, oy = 0.0f, oz = -0.3f;

        logMsg("Port=%s baud=%d", comPort, baud);

        m_controller = new CDIYController(comPort, baud, ox, oy, oz);
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
        if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
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
