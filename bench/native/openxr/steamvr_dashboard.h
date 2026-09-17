#pragma once
#include <windows.h>
#include <winhttp.h>
#include <cstring>
#include <cstdio>
#pragma comment(lib, "winhttp.lib")

// Best-effort SteamVR-specific adapter, verified against the installed 2.17
// dashboard's mailbox protocol. This is NOT a public OpenXR/OpenVR API.
// No settings changes, input simulation, toggle, credentials, or remote hosts.
// A successful send is not an acknowledgement that the dashboard disappeared.
inline DWORD RequestSteamVrDashboardClose()
{
    struct Handle
    {
        HINTERNET value;
        explicit Handle(HINTERNET h) : value(h) {}
        ~Handle() { if (value) WinHttpCloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };
    Handle session(WinHttpOpen(L"VRX/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return GetLastError();
    if (!WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000)) return GetLastError();
    Handle connection(WinHttpConnect(session.value, L"127.0.0.1", 27062, 0));
    if (!connection.value) return GetLastError();
    Handle request(WinHttpOpenRequest(connection.value, L"GET", L"/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request.value) return GetLastError();
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return GetLastError();
    if (!WinHttpSendRequest(request.value, L"Origin: http://localhost:27062\r\n", DWORD(-1),
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) return GetLastError();
    Handle socket(WinHttpWebSocketCompleteUpgrade(request.value, 0));
    if (!socket.value) return GetLastError();
    // Complete the WebSocket close handshake rather than destroying a socket
    // with queued messages. SteamVR otherwise may never dispatch the request.
    DWORD closeTimeout = 1000;
    if (!WinHttpSetOption(socket.value, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
        &closeTimeout, sizeof(closeTimeout))) return GetLastError();
    char mailbox[80];
    std::snprintf(mailbox, sizeof(mailbox), "mailbox_open vrx_startup_%lu", GetCurrentProcessId());
    DWORD result = WinHttpWebSocketSend(socket.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        mailbox, DWORD(std::strlen(mailbox)));
    if (result != ERROR_SUCCESS) return result;
    const char* message = "mailbox_send vrwebui_dashboard {\"type\":\"hide_dashboard_requested\",\"reason\":\"VRX playback started\",\"source_is_vrlink_remote\":false}";
    result = WinHttpWebSocketSend(socket.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message), DWORD(std::strlen(message)));
    if (result != ERROR_SUCCESS) return result;
    return WinHttpWebSocketClose(socket.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
}
