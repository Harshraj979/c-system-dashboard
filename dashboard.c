/* SysDashboard_fixed.c
 * Improved, portable Win32 system dashboard (process list + CPU/memory graphs)
 * Build with: gcc dashboard.c -o dashboard.exe -lcomctl32 -lpsapi -lgdi32 -luser32
 * Works best with MinGW-w64.
 */

#define _WIN32_IE 0x0600
#define _WIN32_WINNT 0x0600
#define WINVER 0x0600

#include <sdkddkver.h>
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

#define ID_LISTVIEW 1001
#define ID_BTN_KILL 1002
#define TIMER_ID 1

#ifndef LVS_EX_DOUBLEBUFFER
#define LVS_EX_DOUBLEBUFFER 0x00010000
#endif

#define HISTORY_SIZE 60

// Globals
HINSTANCE hInst;
HWND hMainWnd, hListView, hBtnKill;
double cpuHistory[HISTORY_SIZE];
double memHistory[HISTORY_SIZE];
int historyMsgIndex = 0;

// CPU Calculation Globals
ULARGE_INTEGER lastIdleTime, lastKernelTime, lastUserTime;

void InitCPU(void) {
    FILETIME idleTime, kernelTime, userTime;
    GetSystemTimes(&idleTime, &kernelTime, &userTime);
    lastIdleTime.LowPart = idleTime.dwLowDateTime;
    lastIdleTime.HighPart = idleTime.dwHighDateTime;
    lastKernelTime.LowPart = kernelTime.dwLowDateTime;
    lastKernelTime.HighPart = kernelTime.dwHighDateTime;
    lastUserTime.LowPart = userTime.dwLowDateTime;
    lastUserTime.HighPart = userTime.dwHighDateTime;
}

static double GetCPUUsage(void) {
    FILETIME idleTime, kernelTime, userTime;
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return 0.0;

    ULARGE_INTEGER idle, kernel, user;
    idle.LowPart = idleTime.dwLowDateTime; idle.HighPart = idleTime.dwHighDateTime;
    kernel.LowPart = kernelTime.dwLowDateTime; kernel.HighPart = kernelTime.dwHighDateTime;
    user.LowPart = userTime.dwLowDateTime; user.HighPart = userTime.dwHighDateTime;

    ULONGLONG idleDiff = idle.QuadPart - lastIdleTime.QuadPart;
    ULONGLONG kernelDiff = kernel.QuadPart - lastKernelTime.QuadPart;
    ULONGLONG userDiff = user.QuadPart - lastUserTime.QuadPart;

    lastIdleTime = idle; lastKernelTime = kernel; lastUserTime = user;

    ULONGLONG total = kernelDiff + userDiff;
    if (total == 0) return 0.0;
    return (double)(total - idleDiff) * 100.0 / (double)total;
}

static double GetMemoryUsage(void) {
    MEMORYSTATUSEX statex;
    memset(&statex, 0, sizeof(statex));
    statex.dwLength = sizeof(statex);
    if (!GlobalMemoryStatusEx(&statex)) return 0.0;
    return (double)statex.dwMemoryLoad; // percent
}

static void UpdateProcessList(void) {
    // Save selected PID if any
    DWORD selectedPid = 0;
    int selIdx = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
    if (selIdx != -1) {
        LVITEM lvSel;
        memset(&lvSel, 0, sizeof(lvSel));
        lvSel.iItem = selIdx; lvSel.iSubItem = 0; lvSel.mask = LVIF_PARAM;
        if (ListView_GetItem(hListView, &lvSel)) selectedPid = (DWORD)lvSel.lParam;
    }

    ListView_DeleteAllItems(hListView);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    int index = 0;
    if (Process32First(hSnapshot, &pe32)) {
        do {
            LVITEM lvItem;
            memset(&lvItem, 0, sizeof(lvItem));
            lvItem.mask = LVIF_TEXT | LVIF_PARAM;
            lvItem.iItem = index;
            lvItem.iSubItem = 0;
            lvItem.pszText = pe32.szExeFile;
            lvItem.lParam = (LPARAM)pe32.th32ProcessID;
            ListView_InsertItem(hListView, &lvItem);

            char buf[64];
            snprintf(buf, sizeof(buf), "%lu", (unsigned long)pe32.th32ProcessID);
            ListView_SetItemText(hListView, index, 1, buf);

            snprintf(buf, sizeof(buf), "%lu", (unsigned long)pe32.cntThreads);
            ListView_SetItemText(hListView, index, 2, buf);

            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
            if (hProcess) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                    snprintf(buf, sizeof(buf), "%.2f MB", (double)pmc.WorkingSetSize / (1024.0 * 1024.0));
                    ListView_SetItemText(hListView, index, 3, buf);
                }
                CloseHandle(hProcess);
            } else {
                ListView_SetItemText(hListView, index, 3, "N/A");
            }

            // Restore selection by PID after insertion
            if (selectedPid && selectedPid == (DWORD)pe32.th32ProcessID) {
                ListView_SetItemState(hListView, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hListView, index, FALSE);
            }

            index++;
        } while (Process32Next(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
}

static void KillSelectedProcess(void) {
    int iPos = ListView_GetNextItem(hListView, -1, LVNI_SELECTED);
    if (iPos == -1) return;

    LVITEM lvItem;
    memset(&lvItem, 0, sizeof(lvItem));
    lvItem.mask = LVIF_PARAM; lvItem.iItem = iPos; lvItem.iSubItem = 0;
    if (!ListView_GetItem(hListView, &lvItem)) return;

    DWORD pid = (DWORD)lvItem.lParam;
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess) {
        TerminateProcess(hProcess, 1);
        CloseHandle(hProcess);
        UpdateProcessList();
    } else {
        MessageBox(hMainWnd, "Failed to terminate process (permission?).", "Error", MB_OK | MB_ICONERROR);
    }
}

static void DrawGraph(HDC hdc, RECT rect, double* history, int historyIdx, COLORREF color, const char* label) {
    // Background
    HBRUSH hBrushBg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rect, hBrushBg);
    DeleteObject(hBrushBg);

    // Grid
    HPEN hPenGrid = CreatePen(PS_DOT, 1, RGB(60, 60, 60));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPenGrid);
    for (int i = 0; i < 4; ++i) {
        int y = rect.top + i * (rect.bottom - rect.top) / 4;
        MoveToEx(hdc, rect.left, y, NULL);
        LineTo(hdc, rect.right, y);
    }
    SelectObject(hdc, hOldPen);
    DeleteObject(hPenGrid);

    // Label
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    TextOutA(hdc, rect.left + 6, rect.top + 4, label, (int)strlen(label));

    char valBuf[32];
    double lastVal = history[(historyIdx - 1 + HISTORY_SIZE) % HISTORY_SIZE];
    snprintf(valBuf, sizeof(valBuf), "%.1f%%", lastVal);
    TextOutA(hdc, rect.right - 60, rect.top + 4, valBuf, (int)strlen(valBuf));

    // Line
    HPEN hPenLine = CreatePen(PS_SOLID, 2, color);
    hOldPen = (HPEN)SelectObject(hdc, hPenLine);

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    double stepX = (double)width / (HISTORY_SIZE - 1);

    for (int i = 0; i < HISTORY_SIZE; ++i) {
        int idx = (historyIdx + i) % HISTORY_SIZE;
        double val = history[idx];
        int x = rect.left + (int)(i * stepX + 0.5);
        int y = rect.bottom - (int)(val / 100.0 * (double)height + 0.5);
        if (i == 0) MoveToEx(hdc, x, y, NULL);
        else LineTo(hdc, x, y);
    }

    SelectObject(hdc, hOldPen);
    DeleteObject(hPenLine);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        hMainWnd = hWnd;
        InitCPU();
        for (int i = 0; i < HISTORY_SIZE; ++i) { cpuHistory[i] = memHistory[i] = 0.0; }

        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icex);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        // Create ListView
        hListView = CreateWindowEx(0, WC_LISTVIEW, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_BORDER,
            0, 200, rcClient.right, rcClient.bottom - 240,
            hWnd, (HMENU)ID_LISTVIEW, hInst, NULL);

        // Extended styles
        ListView_SetExtendedListViewStyle(hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc;
        memset(&lvc, 0, sizeof(lvc));
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt = LVCFMT_LEFT;

        lvc.cx = 240; lvc.pszText = "Name"; ListView_InsertColumn(hListView, 0, &lvc);
        lvc.cx = 80;  lvc.pszText = "PID";  ListView_InsertColumn(hListView, 1, &lvc);
        lvc.cx = 80;  lvc.pszText = "Threads"; ListView_InsertColumn(hListView, 2, &lvc);
        lvc.cx = 120; lvc.pszText = "Memory";  ListView_InsertColumn(hListView, 3, &lvc);

        // Button
        hBtnKill = CreateWindowA("BUTTON", "End Selection",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            10, rcClient.bottom - 35, 120, 30,
            hWnd, (HMENU)ID_BTN_KILL, hInst, NULL);

        SetTimer(hWnd, TIMER_ID, 1000, NULL);
        UpdateProcessList();
        break;
    }

    case WM_SIZE: {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        MoveWindow(hListView, 0, 200, rcClient.right, rcClient.bottom - 240, TRUE);
        MoveWindow(hBtnKill, 10, rcClient.bottom - 35, 120, 30, TRUE);
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_BTN_KILL) KillSelectedProcess();
        break;

    case WM_TIMER: {
        double cpu = GetCPUUsage();
        double mem = GetMemoryUsage();

        cpuHistory[historyMsgIndex] = cpu;
        memHistory[historyMsgIndex] = mem;
        historyMsgIndex = (historyMsgIndex + 1) % HISTORY_SIZE;

        static int listTick = 0;
        if (++listTick >= 8) { UpdateProcessList(); listTick = 0; }

        // Invalidate only top area (graphs)
        RECT rc = {0, 0, 2000, 200};
        InvalidateRect(hWnd, &rc, FALSE);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        RECT rcGraph = rcClient; rcGraph.bottom = 200;

        // Double buffering
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rcGraph.right, rcGraph.bottom);
        HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        // Fill background
        HBRUSH hbr = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdcMem, &rcGraph, hbr);
        DeleteObject(hbr);

        RECT rcCPU = {rcGraph.left, rcGraph.top, rcGraph.right/2, rcGraph.bottom};
        RECT rcMem = {rcGraph.right/2, rcGraph.top, rcGraph.right, rcGraph.bottom};

        DrawGraph(hdcMem, rcCPU, cpuHistory, historyMsgIndex, RGB(0,200,0), "CPU Usage");
        DrawGraph(hdcMem, rcMem, memHistory, historyMsgIndex, RGB(0,150,255), "Memory Usage");

        BitBlt(hdc, 0, 0, rcGraph.right, rcGraph.bottom, hdcMem, 0, 0, SRCCOPY);

        SelectObject(hdcMem, hOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    hInst = hInstance;

    // Init common controls before creating windows
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEX wcex;
    memset(&wcex, 0, sizeof(wcex));
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = "SysDashClass";
    wcex.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassEx(&wcex)) {
        MessageBox(NULL, "Call to RegisterClassEx failed!", "Error", MB_OK);
        return 1;
    }

    hMainWnd = CreateWindowA("SysDashClass", "System Dashboard C",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL, NULL, hInstance, NULL);

    if (!hMainWnd) {
        MessageBox(NULL, "Call to CreateWindow failed!", "Error", MB_OK);
        return 1;
    }

    ShowWindow(hMainWnd, nCmdShow);
    UpdateWindow(hMainWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
