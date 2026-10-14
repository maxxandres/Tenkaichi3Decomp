#include "discord_ipc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <stdint.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <process.h>
  static HANDLE sPipeHandle = INVALID_HANDLE_VALUE;
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
  #include <unistd.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <fcntl.h>
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif
  static int sSockFd = -1;
#endif

static bool sConnected = false;
static char sAppId[64] = {0};
static time_t sStartTime = 0;

// Discord IPC standard opcodes
enum DiscordOpcode {
    OP_HANDSHAKE = 0,
    OP_FRAME     = 1,
    OP_CLOSE     = 2,
    OP_PING      = 3,
    OP_PONG      = 4
};

// Binary header prefix prepended to all Discord IPC frames
#pragma pack(push, 1)
struct DiscordHeader {
    int32_t opcode;
    int32_t length;
};
#pragma pack(pop)

static bool WritePacket(int32_t opcode, const char *payload) {
    if (!sConnected) return false;

    DiscordHeader header;
    header.opcode = opcode;
    header.length = (int32_t)strlen(payload);

#if defined(_WIN32)
    DWORD written = 0;
    if (!WriteFile(sPipeHandle, &header, sizeof(header), &written, NULL) ||
        !WriteFile(sPipeHandle, payload, header.length, &written, NULL)) {
        sConnected = false;
        CloseHandle(sPipeHandle);
        sPipeHandle = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    if (send(sSockFd, &header, sizeof(header), MSG_NOSIGNAL) != (ssize_t)sizeof(header) ||
        send(sSockFd, payload, header.length, MSG_NOSIGNAL) != (ssize_t)header.length) {
        sConnected = false;
        close(sSockFd);
        sSockFd = -1;
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool Discord_IsConnected(void) {
    return sConnected;
}

void Discord_Init(const char *application_id) {
    if (sConnected) return;

    // Skip initialization during headless builds or automated demo runs
    if (getenv("BT3_GS") && strcmp(getenv("BT3_GS"), "none") == 0) return;
    if (getenv("BT3_DEMO") != NULL) return;

    if (application_id && application_id[0] != '\0') {
        strncpy(sAppId, application_id, sizeof(sAppId) - 1);
        sAppId[sizeof(sAppId) - 1] = '\0';
    }
    if (sAppId[0] == '\0') return;
    sStartTime = time(NULL);

#if defined(_WIN32)
    char pipeName[64];
    for (int i = 0; i < 10; i++) {
        snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\discord-ipc-%d", i);
        sPipeHandle = CreateFileA(pipeName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (sPipeHandle != INVALID_HANDLE_VALUE) {
            sConnected = true;
            break;
        }
    }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    const char *runtimeDir = getenv("XDG_RUNTIME_DIR");
    const char *tmpDir = getenv("TMPDIR");
    if (!tmpDir) tmpDir = "/tmp";

    char flatpakDir[256] = {0};
    char snapDir[256] = {0};
    if (runtimeDir) {
        snprintf(flatpakDir, sizeof(flatpakDir), "%s/app/com.discordapp.Discord", runtimeDir);
        snprintf(snapDir, sizeof(snapDir), "%s/snap.discord", runtimeDir);
    }

    const char *candidateDirs[4];
    int numCandidates = 0;
    if (runtimeDir) candidateDirs[numCandidates++] = runtimeDir;
    if (flatpakDir[0]) candidateDirs[numCandidates++] = flatpakDir;
    if (snapDir[0]) candidateDirs[numCandidates++] = snapDir;
    candidateDirs[numCandidates++] = tmpDir;

    for (int d = 0; d < numCandidates && !sConnected; d++) {
        for (int i = 0; i < 10; i++) {
            char sockPath[512];
            snprintf(sockPath, sizeof(sockPath), "%s/discord-ipc-%d", candidateDirs[d], i);

            sSockFd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sSockFd < 0) continue;

            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);

            if (connect(sSockFd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
#if defined(SO_NOSIGPIPE)
                int opt = 1;
                setsockopt(sSockFd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
                sConnected = true;
                break;
            }
            close(sSockFd);
            sSockFd = -1;
        }
    }
#endif

    if (!sConnected) return;

    // Send initial handshake JSON
    char handshakePayload[256];
    snprintf(handshakePayload, sizeof(handshakePayload),
             "{\"v\":1,\"client_id\":\"%s\"}", sAppId);

    WritePacket(OP_HANDSHAKE, handshakePayload);
}

void Discord_Update(const char *state, const char *details, const char *large_image_key, const char *large_image_text) {
    Discord_UpdateEx(state, details, large_image_key, large_image_text, (long long)sStartTime);
}

void Discord_UpdateEx(const char *state, const char *details, const char *large_image_key, const char *large_image_text, long long start_timestamp) {
    Discord_UpdateEx2(state, details, large_image_key, large_image_text, NULL, NULL, start_timestamp);
}

void Discord_UpdateEx2(const char *state, const char *details,
                       const char *large_image_key, const char *large_image_text,
                       const char *small_image_key, const char *small_image_text,
                       long long start_timestamp) {
    if (!sConnected) return;

    static int sNonce = 1;
    char json[1024];

#if defined(_WIN32)
    int currentPid = (int)GetCurrentProcessId();
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    int currentPid = (int)getpid();
#else
    int currentPid = 0;
#endif

    char assetsJson[512];
    if (small_image_key && small_image_key[0] != '\0') {
        snprintf(assetsJson, sizeof(assetsJson),
            "\"assets\":{"
                "\"large_image\":\"%s\","
                "\"large_text\":\"%s\","
                "\"small_image\":\"%s\","
                "\"small_text\":\"%s\""
            "}",
            large_image_key ? large_image_key : "",
            large_image_text ? large_image_text : "",
            small_image_key,
            small_image_text ? small_image_text : ""
        );
    } else {
        snprintf(assetsJson, sizeof(assetsJson),
            "\"assets\":{"
                "\"large_image\":\"%s\","
                "\"large_text\":\"%s\""
            "}",
            large_image_key ? large_image_key : "",
            large_image_text ? large_image_text : ""
        );
    }

    if (start_timestamp > 0) {
        snprintf(json, sizeof(json),
            "{"
                "\"cmd\":\"SET_ACTIVITY\","
                "\"args\":{"
                    "\"pid\":%d,"
                    "\"activity\":{"
                        "\"state\":\"%s\","
                        "\"details\":\"%s\","
                        "\"timestamps\":{\"start\":%lld},"
                        "%s"
                    "}"
                "},"
                "\"nonce\":\"%d\""
            "}",
            currentPid,
            state ? state : "",
            details ? details : "",
            start_timestamp,
            assetsJson,
            sNonce++
        );
    } else {
        snprintf(json, sizeof(json),
            "{"
                "\"cmd\":\"SET_ACTIVITY\","
                "\"args\":{"
                    "\"pid\":%d,"
                    "\"activity\":{"
                        "\"state\":\"%s\","
                        "\"details\":\"%s\","
                        "%s"
                    "}"
                "},"
                "\"nonce\":\"%d\""
            "}",
            currentPid,
            state ? state : "",
            details ? details : "",
            assetsJson,
            sNonce++
        );
    }

    WritePacket(OP_FRAME, json);
}

void Discord_RunCallbacks(void) {
    if (!sConnected) return;

    char buffer[512];
#if defined(_WIN32)
    DWORD bytesAvailable = 0;
    if (PeekNamedPipe(sPipeHandle, NULL, 0, NULL, &bytesAvailable, NULL) && bytesAvailable > 0) {
        DWORD readBytes = 0;
        ReadFile(sPipeHandle, buffer, sizeof(buffer), &readBytes, NULL);
    }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    ssize_t n = recv(sSockFd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (n == 0) {
        sConnected = false;
        close(sSockFd);
        sSockFd = -1;
    }
#endif
}

void Discord_Shutdown(void) {
    if (!sConnected) return;

    WritePacket(OP_CLOSE, "{}");

#if defined(_WIN32)
    if (sPipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(sPipeHandle);
        sPipeHandle = INVALID_HANDLE_VALUE;
    }
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__)
    if (sSockFd >= 0) {
        close(sSockFd);
        sSockFd = -1;
    }
#endif
    sConnected = false;
}