
#ifdef _WIN32

#include "../volumeoptions/sound_plugin.h" // TODO: separate implementation from interface to not include asio here.

#include <Windows.h>
#include <minmax.h> // for max
#include <CommCtrl.h>
#include <tchar.h>
#include "../windows_resources/gui_resource.h"

#include <string>

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "ComCtl32.lib")

/*
    Initializes Dialog controls values from VolumeOptions settings
*/
void InitControlValues(HWND hDlg, const vo::volume_options_settings& vo_settings)
{
    // settings shortcuts
    const vo::session_settings& ses_settings = vo_settings.monitor_settings.ses_global_settings;
    const vo::monitor_settings& mon_settings = vo_settings.monitor_settings;

    // ------ TS3 settings

    if (vo_settings.exclude_own_client)
        CheckDlgButton(hDlg, IDC_CHECK_EXCLUDEOWNCLIENT, BST_CHECKED);
    else
        CheckDlgButton(hDlg, IDC_CHECK_EXCLUDEOWNCLIENT, BST_UNCHECKED);


    // ------ Audio Monitor settings

    if (mon_settings.use_included_filter)
        CheckRadioButton(hDlg, IDC_RADIO_EXCLUDEFILTER, IDC_RADIO_INCLUDEFILTER, IDC_RADIO_INCLUDEFILTER);
    else
        CheckRadioButton(hDlg, IDC_RADIO_EXCLUDEFILTER, IDC_RADIO_INCLUDEFILTER, IDC_RADIO_EXCLUDEFILTER);

    std::wstring included_process_list;
    for (const auto p : mon_settings.included_process)
        included_process_list += (p + L";");

    std::wstring excluded_process_list;
    for (const auto p : mon_settings.excluded_process)
        excluded_process_list += (p + L";");

    SetDlgItemText(hDlg, IDC_EDIT_INCLUDEFILTER, included_process_list.c_str());
    SetDlgItemText(hDlg, IDC_EDIT_EXCLUDEFILTER, excluded_process_list.c_str());


    if (mon_settings.exclude_own_process)
        CheckDlgButton(hDlg, IDC_CHECK_EXCLUDEOWNPROCESS, BST_CHECKED);
    else
        CheckDlgButton(hDlg, IDC_CHECK_EXCLUDEOWNPROCESS, BST_UNCHECKED);


    // ------ Audio Monitor Session settings

    unsigned int slider_level = 0;
    if (ses_settings.treat_vol_as_percentage)
    {
        slider_level = unsigned int(ses_settings.vol_reduction * 100);
        CheckRadioButton(hDlg, IDC_RADIO_VOLUMEFIXED, IDC_RADIO_VOLUMEPERCENTAGE, IDC_RADIO_VOLUMEPERCENTAGE);
    }
    else
    {
        slider_level = unsigned int(ses_settings.vol_reduction * 100); // TODO another slider
        CheckRadioButton(hDlg, IDC_RADIO_VOLUMEFIXED, IDC_RADIO_VOLUMEPERCENTAGE, IDC_RADIO_VOLUMEFIXED);
    }
    HWND hVolSlider = GetDlgItem(hDlg, IDC_SLIDER_VOLUMELEVEL);
    SendMessage(hVolSlider, TBM_SETPOS, TRUE, slider_level);

    if (ses_settings.change_only_active_sessions)
        CheckDlgButton(hDlg, IDC_CHECK_APPLYONLYACTIVE, BST_CHECKED);
    else
        CheckDlgButton(hDlg, IDC_CHECK_APPLYONLYACTIVE, BST_UNCHECKED);

    std::wstring delay_wstring = L"0";
    if (ses_settings.vol_up_delay.count() != 0)
        delay_wstring = std::to_wstring(ses_settings.vol_up_delay.count());
    SetDlgItemText(hDlg, IDC_EDIT_DELAYTIME, delay_wstring.c_str());

}

void UpdateSettings(HWND hDlg, vo::volume_options_settings& vo_settings)
{
    // settings shortcuts
    vo::session_settings& ses_settings = vo_settings.monitor_settings.ses_global_settings;
    vo::monitor_settings& mon_settings = vo_settings.monitor_settings;

    UINT state;


    state = IsDlgButtonChecked(hDlg, IDC_CHECK_EXCLUDEOWNCLIENT);
    if (state == BST_CHECKED)
        vo_settings.exclude_own_client = true;
    else
        vo_settings.exclude_own_client = false;


    // ------- Retrieve Session settings group

    // TODO: get slider value..

    state = IsDlgButtonChecked(hDlg, IDC_CHECK_APPLYONLYACTIVE);
    if (state == BST_CHECKED)
        ses_settings.change_only_active_sessions = true;
    else
        ses_settings.change_only_active_sessions = false;

    BOOL ret;
    UINT delay_milliseconds = GetDlgItemInt(hDlg, IDC_EDIT_DELAYTIME, &ret, FALSE); // use uint version
    if (ret)
    {
        ses_settings.vol_up_delay = std::chrono::milliseconds(delay_milliseconds);
        dwprintf(L"ses_settings.vol_up_delay: %u\n", delay_milliseconds);
    }


    // ------- Retrieve Monitor settings group
    const UINT LIST_MAX_SIZE = 4096;
    wchar_t process_names[LIST_MAX_SIZE] = { 0 };

    state = IsDlgButtonChecked(hDlg, IDC_CHECK_EXCLUDEOWNPROCESS);
    if (state == BST_CHECKED)
        mon_settings.exclude_own_process = true;
    else
        mon_settings.exclude_own_process = false;

    state = IsDlgButtonChecked(hDlg, IDC_RADIO_EXCLUDEFILTER);
    if (state == BST_CHECKED)
        mon_settings.use_included_filter = false;

    state = IsDlgButtonChecked(hDlg, IDC_RADIO_INCLUDEFILTER);
    if (state == BST_CHECKED)
        mon_settings.use_included_filter = true;

    // Parse list and insert it to monitor settings sets
    mon_settings.included_process.clear();
    mon_settings.excluded_process.clear();
    if (mon_settings.use_included_filter)
    {
        memset(process_names, 0, LIST_MAX_SIZE);
        GetDlgItemText(hDlg, IDC_EDIT_INCLUDEFILTER, process_names, LIST_MAX_SIZE);
        vo::parse_process_list(process_names, mon_settings.included_process);
        dwprintf(L"excluded process_names: %s\n\n", process_names);
    }
    if (!mon_settings.use_included_filter)
    {
        memset(process_names, 0, LIST_MAX_SIZE);
        GetDlgItemText(hDlg, IDC_EDIT_EXCLUDEFILTER, process_names, LIST_MAX_SIZE);
        vo::parse_process_list(process_names, mon_settings.excluded_process);
        dwprintf(L"included process_names: %s\n\n", process_names);
    }

}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static vo::VolumeOptions* pvo = nullptr;
    static vo::volume_options_settings vo_settings;

    switch (uMsg)
    {

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDCANCEL:
            SendMessage(hDlg, WM_CLOSE, 0, 0);
            return TRUE;

        case IDOK:
            UpdateSettings(hDlg, vo_settings);
            pvo->set_settings(vo_settings);
            SendMessage(hDlg, WM_CLOSE, 0, 0);
            return TRUE;

        case IDC_CHECK_EXCLUDEOWNCLIENT:
            return TRUE;

        case IDC_CHECK_APPLYONLYACTIVE:
            return TRUE;

        case IDC_RADIO_VOLUMEFIXED: case IDC_RADIO_VOLUMEPERCENTAGE:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                wprintf(L"BN_CLICKED\n");
            }
            return TRUE;

        case IDC_CHECK_EXCLUDEOWNPROCESS:
            return TRUE;

        case IDC_RADIO_EXCLUDEFILTER: case IDC_RADIO_INCLUDEFILTER:
            return TRUE;

        default:
            return FALSE;
        }
    }
    break;

    case WM_VSCROLL:
        switch (LOWORD(wParam))
        {
        case SB_THUMBPOSITION:
            wprintf(L"pos level = %d\n", HIWORD(wParam));
            break;

        case SB_THUMBTRACK:
            wprintf(L"track level = %d\n", HIWORD(wParam));
            break;

        }
        wprintf(L"what lo=%d hi=%d\n", LOWORD(wParam), HIWORD(wParam));
        return 0;

    case  WM_INITDIALOG:
    {
        pvo = (vo::VolumeOptions*)lParam;
        vo_settings = pvo->get_current_settings();

        InitControlValues(hDlg, vo_settings);

        return TRUE;
    }
    break;

    case WM_CLOSE:
    {
        DestroyWindow(hDlg);
        return TRUE;
    }
    break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;

    } // end switch

    return FALSE;
}

int DialogThread(vo::VolumeOptions* pvo)
{
    HWND hDlg;
    MSG msg;
    BOOL ret;

    int nCmdShow = SW_SHOW;
    HINSTANCE hInst = NULL;

    InitCommonControls();
    hDlg = CreateDialogParam(hInst, MAKEINTRESOURCE(IDD_VO_CONFIG_DIALOG), 0, DialogProc, LPARAM(pvo));
    ShowWindow(hDlg, nCmdShow);

    while (ret = GetMessage(&msg, 0, 0, 0))
    {
        if (ret == -1)
            return -1;

        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int)msg.wParam;
}












// TESTING..............

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

static BOOL CALLBACK DlgProc(HWND, UINT, WPARAM, LPARAM);
static void DrawPeakMeter(HWND, float);

// Timer ID and period (in milliseconds)
#define ID_TIMER  1
#define TIMER_PERIOD  50

#define EXIT_ON_ERROR(hr)  \
              if (FAILED(hr)) { goto Exit; }
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(punk)  \
              if ((punk) != NULL)  \
                              { (punk)->Release(); (punk) = NULL; }
#endif

//-----------------------------------------------------------
// WinMain -- Opens a dialog box that contains a peak meter.
//   The peak meter displays the peak sample value that plays
//   through the default rendering device.
//-----------------------------------------------------------
int DialogThread2()
{
    HRESULT hr;
    IMMDeviceEnumerator *pEnumerator = NULL;
    IMMDevice *pDevice = NULL;
    IAudioMeterInformation *pMeterInfo = NULL;


    CoInitialize(NULL);

    // Get enumerator for audio endpoint devices.
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
        NULL, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),
        (void**)&pEnumerator);
    EXIT_ON_ERROR(hr)

        // Get peak meter for default audio-rendering device.
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    EXIT_ON_ERROR(hr)

        hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
        CLSCTX_ALL, NULL, (void**)&pMeterInfo);
    EXIT_ON_ERROR(hr)

        DialogBoxParam(NULL, L"PEAKMETER", NULL, (DLGPROC)DlgProc, (LPARAM)pMeterInfo);

Exit:
    if (FAILED(hr))
    {
        MessageBox(NULL, TEXT("This program requires Windows Vista."),
            TEXT("Error termination"), MB_OK);
    }
    SAFE_RELEASE(pEnumerator)
        SAFE_RELEASE(pDevice)
        SAFE_RELEASE(pMeterInfo)
        CoUninitialize();
    return 0;
}

//-----------------------------------------------------------
// DlgProc -- Dialog box procedure
//-----------------------------------------------------------

BOOL CALLBACK DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    static IAudioMeterInformation *pMeterInfo = NULL;
    static HWND hPeakMeter = NULL;
    static float peak = 0;
    HRESULT hr;

    switch (message)
    {
    case WM_INITDIALOG:
        pMeterInfo = (IAudioMeterInformation*)lParam;
        SetTimer(hDlg, ID_TIMER, TIMER_PERIOD, NULL);
        hPeakMeter = GetDlgItem(hDlg, IDC_PEAK_METER);
        return TRUE;

    case WM_COMMAND:
        switch ((int)LOWORD(wParam))
        {
        case IDCANCEL:
            KillTimer(hDlg, ID_TIMER);
            EndDialog(hDlg, TRUE);
            return TRUE;
        }
        break;

    case WM_TIMER:
        switch ((int)wParam)
        {
        case ID_TIMER:
            // Update the peak meter in the dialog box.
            hr = pMeterInfo->GetPeakValue(&peak);
            if (FAILED(hr))
            {
                MessageBox(hDlg, TEXT("The program will exit."),
                    TEXT("Fatal error"), MB_OK);
                KillTimer(hDlg, ID_TIMER);
                EndDialog(hDlg, TRUE);
                return TRUE;
            }
            DrawPeakMeter(hPeakMeter, peak);
            return TRUE;
        }
        break;

    case WM_PAINT:
        // Redraw the peak meter in the dialog box.
        ValidateRect(hPeakMeter, NULL);
        DrawPeakMeter(hPeakMeter, peak);
        break;
    }
    return FALSE;
}

//-----------------------------------------------------------
// DrawPeakMeter -- Draws the peak meter in the dialog box.
//-----------------------------------------------------------

#define DRAWPEAK_METER_HORIZONTAL 0 // 0 = vertical, from bottom to top
void DrawPeakMeter(HWND hPeakMeter, float peak)
{
    HDC hdc;
    RECT rect;
#if DRAWPEAK_METER_HORIZONTAL
    GetClientRect(hPeakMeter, &rect);
    hdc = GetDC(hPeakMeter);
    FillRect(hdc, &rect, (HBRUSH)(COLOR_3DSHADOW + 1));
    rect.left++;
    rect.top++;
    rect.right = rect.left +
        max(0, (LONG)(peak*(rect.right - rect.left) - 1.5));
    rect.bottom--;
    FillRect(hdc, &rect, (HBRUSH)(COLOR_3DHIGHLIGHT + 1));
    ReleaseDC(hPeakMeter, hdc);
#else
    GetClientRect(hPeakMeter, &rect);
    hdc = GetDC(hPeakMeter);
    FillRect(hdc, &rect, (HBRUSH)(COLOR_3DSHADOW + 1));
    rect.left++;
    rect.right--;
    rect.bottom--;
    rect.top = rect.bottom -
        max(0, (LONG)(peak*(rect.top + rect.bottom) + 1.5));

    FillRect(hdc, &rect, (HBRUSH)(COLOR_3DHIGHLIGHT + 1));
    ReleaseDC(hPeakMeter, hdc);
#endif
}


#endif