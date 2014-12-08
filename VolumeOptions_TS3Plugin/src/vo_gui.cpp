
#ifdef _WIN32

#include "../volumeoptions/config.h"
#include "../volumeoptions/vo_ts3plugin.h" // TODO: separate implementation from interface to not include asio here.

#include <windows.h>
#include <minmax.h> // for max
#include <commctrl.h>
#include <tchar.h>
#include "../resources/gui_resource.h"

#include <string>
#include <unordered_map> // to save opened windows

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma comment(lib, "ComCtl32.lib")

// 0 for modal, 1 for modeless.
#define VO_MODELESS_DIALOG_GUI 1

// Saved opened HWND windows, to easily close them
struct DialogID
{
    HWND hDlg;
    DWORD thread_id;
};
static std::unordered_map<DWORD, DialogID> g_opened_hwnds;
void SaveOpenedDialog(HWND handle)
{
    DialogID id;
    id.hDlg = handle;
    id.thread_id = GetCurrentThreadId();
    g_opened_hwnds[id.thread_id] = id;
}
void DeleteSavedDialog(HWND handle)
{
    DialogID id;
    id.hDlg = handle;
    id.thread_id = GetCurrentThreadId();
    g_opened_hwnds.erase(id.thread_id);
}
void DestroyAllOpenedDialogs()
{
    for (auto h : g_opened_hwnds)
        PostThreadMessage(h.second.thread_id, WM_QUIT, 0, 0);

    g_opened_hwnds.clear();
}

// 3 ways to get current process module load base address. HINSTANCE HMODULE POINTER
HMODULE GetCurrentModule()
{
    HMODULE hModule = NULL;
    GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCTSTR>(GetCurrentModule), &hModule);

    return hModule;
}
HMODULE GetCurrentModule_ver2()
{
    static int s_somevar = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!::VirtualQuery(&s_somevar, &mbi, sizeof(mbi)))
    {
        return NULL;
    }
    return static_cast<HMODULE>(mbi.AllocationBase);
}
#ifdef _MSC_VER
// Base address from PE header. HINSTANCE are just pointers to base module address
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)
#endif

// Helper class to control activation context
// http://msdn.microsoft.com/en-us/library/aa375197.aspx
class CActivationContext
{
    HANDLE m_hActivationContext;

public:
    CActivationContext() : m_hActivationContext(INVALID_HANDLE_VALUE)
    {}

    VOID Set(HANDLE hActCtx)
    {
        if (hActCtx != INVALID_HANDLE_VALUE)
            AddRefActCtx(hActCtx);

        if (m_hActivationContext != INVALID_HANDLE_VALUE)
            ReleaseActCtx(m_hActivationContext);
        m_hActivationContext = hActCtx;
    }

    ~CActivationContext()
    {
        if (m_hActivationContext != INVALID_HANDLE_VALUE)
            ReleaseActCtx(m_hActivationContext);
    }

    BOOL Activate(ULONG_PTR &ulpCookie)
    {
        return ActivateActCtx(m_hActivationContext, &ulpCookie);
    }

    BOOL Deactivate(ULONG_PTR ulpCookie)
    {
        return DeactivateActCtx(0, ulpCookie);
    }
};

class CActCtxActivator
{
    CActivationContext &m_ActCtx;
    ULONG_PTR m_Cookie;
    bool m_fActivated;

public:
    CActCtxActivator(CActivationContext& src, bool fActivate = true)
        : m_ActCtx(src), m_Cookie(0), m_fActivated(false)
    {
        if (fActivate) {
            if (src.Activate(m_Cookie))
                m_fActivated = true;
        }
    }

    ~CActCtxActivator()
    {
        if (m_fActivated) {
            m_ActCtx.Deactivate(m_Cookie);
            m_fActivated = false;
        }
    }
};

CActivationContext g_context; // global, to isolate this com object if loaded from dll.

// http://msdn.microsoft.com/en-us/library/aa375197.aspx
// Modified to use our auto linked manifest
void InitIsolationAware(HINSTANCE hInst)
{
    HANDLE hActCtx;
    ACTCTX actctx = { sizeof(actctx) };
    actctx.hModule = hInst;
    actctx.lpResourceName = ISOLATIONAWARE_MANIFEST_RESOURCE_ID; // our pragma uses resource 2 it seems.
    actctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    hActCtx = CreateActCtx(&actctx);
    g_context.Set(hActCtx);
    ReleaseActCtx(hActCtx);
    // The static destructor for g_context destroys the
    // activation context on component (dll, module, etc) unload.
}

// Description:
//   Creates a tooltip for an item in a dialog box. 
//      tuned for volumeoptions config dialog.
// Parameters:
//   idTool - identifier of an dialog box item.
//   nDlg - window handle of the dialog box.
//   pszText - string to use as the tooltip text.
// Returns:
//   The handle to the tooltip.
//
HWND CreateToolTip(int toolID, HWND hDlg, PTSTR pszText)
{
    HINSTANCE hinst = GetModuleHandle(NULL);

    if (!toolID || !hDlg || !pszText)
    {
        return FALSE;
    }
    // Get the window of the tool.
    HWND hwndTool = GetDlgItem(hDlg, toolID);

    // Create the tooltip. hinst is the global instance handle.
    HWND hwndTip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        hDlg, NULL,
        hinst, NULL);

    if (!hwndTool || !hwndTip)
    {
        return (HWND)NULL;
    }

    // Associate the tooltip with the tool.
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hDlg;
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_PARSELINKS;//| TTF_CENTERTIP;
    toolInfo.uId = (UINT_PTR)hwndTool;
    toolInfo.lpszText = pszText;
    SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

    // Fix ugly windows tooltip time settings. (wish every software did this)
    WORD milliseconds = 0x7FFF; // its signed.. what? ok windows.
    DWORD lParam = milliseconds;
    SendMessage(hwndTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, lParam);
    SendMessage(hwndTip, TTM_SETDELAYTIME, TTDT_INITIAL, 0);
    SendMessage(hwndTip, TTM_SETDELAYTIME, TTDT_RESHOW, 0);

    // Enable multiline tolltip
    SendMessage(hwndTip, TTM_SETMAXTIPWIDTH, 0, MAXINT);

    return hwndTip; 
}

/*
    Centers HWND window to parent window if it has one
*/
BOOL CenterWindow(HWND hwndWindow)
{
    HWND hwndParent;
    RECT rectWindow, rectParent;

    // make the window relative to its parent
    if ((hwndParent = GetParent(hwndWindow)) != NULL)
    {
        GetWindowRect(hwndWindow, &rectWindow);
        GetWindowRect(hwndParent, &rectParent);

        int nWidth = rectWindow.right - rectWindow.left;
        int nHeight = rectWindow.bottom - rectWindow.top;

        int nX = ((rectParent.right - rectParent.left) - nWidth) / 2 + rectParent.left;
        int nY = ((rectParent.bottom - rectParent.top) - nHeight) / 2 + rectParent.top;

        int nScreenWidth = GetSystemMetrics(SM_CXSCREEN);
        int nScreenHeight = GetSystemMetrics(SM_CYSCREEN);

        // make sure that the dialog box never moves outside of the screen
        if (nX < 0) nX = 0;
        if (nY < 0) nY = 0;
        if (nX + nWidth > nScreenWidth) nX = nScreenWidth - nWidth;
        if (nY + nHeight > nScreenHeight) nY = nScreenHeight - nHeight;

        MoveWindow(hwndWindow, nX, nY, nWidth, nHeight, FALSE);

        return TRUE;
    }

    return FALSE;
}

/*
    Sets tooltip descriptions for volumeoptions config dialog
*/
void SetControlTooltips(HWND hDlg)
{
    // Tooltip values:

    CreateToolTip(IDC_CHECK_EXCLUDEOWNCLIENT, hDlg,
        L"Ignore volume reduction when we talk?\r\n"
        L"Default: Checked (true)");

    CreateToolTip(IDC_CHECK_APPLYONLYACTIVE, hDlg,
        L"Change volume only to active (playing sound) audio sessions?\r\n"
        L"Default: Checked (true) (recommended)");

    CreateToolTip(IDC_EDIT_DELAYTIME, hDlg,
        L"Time in milliseconds to delay sessions volume restore\r\n"
        L" when no one is talking.\r\n"
        L"Default: 400ms");

    CreateToolTip(IDC_CHECK_EXCLUDEOWNPROCESS, hDlg,
        L"In this case, exclude Team Speak? this should be true in most cases.\r\n"
        L"Default: Checked (true)");

    CreateToolTip(IDC_RADIO_EXCLUDEFILTER, hDlg,
        L"Excluded process and Included process takes a list of executable names, paths or SIIDs\r\n"
        L"\r\n"
        L"Format: A list of process names separated by \";\"\r\n"
        L"  in case of process names, can be anything, from full path to process names.\r\n"
        L"\r\n"
        L"Example:\r\n"
        L"  process1.exe;X:\\this\\path\\to\\my\\program; _player;  \\this\\directory\r\n");

    CreateToolTip(IDC_CHECK_VOLUMEASPERCENTAGE, hDlg,
        L"Cheked: Take volume value as %.\r\n"
        L"Uncheked: Take volume level as fixed value.\r\n"
        L"Default: Checked (%)\r\n"
        L"\r\n"
        L"Care with fixed values, it can actually increase volume if\r\n"
        L" some apps have volume level lower than max.");

}

/*
    Updates the marker for current position of the volume reduction track bar
*/
void UpdateSliderStaticLabel(HWND hDlg, HWND hVolSlider)
{
    // Update slider current value
    int value = static_cast<int>(SendMessage(hVolSlider, TBM_GETPOS, 0, 0));
    std::wstring ws_value = std::to_wstring(value);
    SetDlgItemText(hDlg, IDC_STATIC_SLIDER_CURRENTVAL, ws_value.c_str());

    HWND hStaticWarnNote = GetDlgItem(hDlg, IDC_STATIC_NOTE_WARNING_VOL);
    if (value < 0)
    {
        ShowWindow(hStaticWarnNote, SW_SHOW);
    }
    else
    {
        ShowWindow(hStaticWarnNote, SW_HIDE);
    }
}

/*
    Updates and Switch all the controls associated with the volume trackbar
        if it tracks percentage or fixed values.
*/
void UpdateSliderControls(HWND hDlg)
{
    HWND hVolSlider = GetDlgItem(hDlg, IDC_SLIDER_VOLUMELEVEL);

    if (BST_CHECKED == IsDlgButtonChecked(hDlg, IDC_CHECK_VOLUMEASPERCENTAGE))
    {
        SetDlgItemText(hDlg, IDC_STATIC_SLIDERMIN, L"-100%");
        SetDlgItemText(hDlg, IDC_STATIC_SLIDERMAX, L"100%");

        // Set range
        DWORD range = MAKELONG(-100, 100);
        SendMessage(hVolSlider, TBM_SETRANGE, TRUE, range);

        SendMessage(hVolSlider, TBM_SETTIC, 0, 0); // tick at 0
    }
    else
    {
        SetDlgItemText(hDlg, IDC_STATIC_SLIDERMIN, L"0 ");
        SetDlgItemText(hDlg, IDC_STATIC_SLIDERMAX, L"100 ");

        SendMessage(hVolSlider, TBM_CLEARTICS, FALSE, 0);

       // LONG style = GetWindowLong(hVolSlider, GWL_STYLE);
     //   style = (style & ~TBS_LEFT) | TBS_RIGHT | TBS_REVERSED | TBS_DOWNISLEFT;
     //   SetWindowLong(hVolSlider, GWL_STYLE, style);

        // Set range (we will flip)
        DWORD range = MAKELONG(0, 100);
        SendMessage(hVolSlider, TBM_SETRANGE, TRUE, range);
    }

    UpdateSliderStaticLabel(hDlg, hVolSlider);
}

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

    HWND include_edit = GetDlgItem(hDlg, IDC_EDIT_INCLUDEFILTER);
    HWND exclude_edit = GetDlgItem(hDlg, IDC_EDIT_EXCLUDEFILTER);
    if (mon_settings.use_included_filter)
    {
        EnableWindow(include_edit, TRUE);
        EnableWindow(exclude_edit, FALSE);
    }
    else
    {
        EnableWindow(include_edit, FALSE);
        EnableWindow(exclude_edit, TRUE);
    }

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

    if (ses_settings.treat_vol_as_percentage)
        CheckDlgButton(hDlg, IDC_CHECK_VOLUMEASPERCENTAGE, BST_CHECKED);
    else
        CheckDlgButton(hDlg, IDC_CHECK_VOLUMEASPERCENTAGE, BST_UNCHECKED);

    UpdateSliderControls(hDlg);
    int slider_level = int(ses_settings.vol_reduction * 100);
    HWND hVolSlider = GetDlgItem(hDlg, IDC_SLIDER_VOLUMELEVEL);
    SendMessage(hVolSlider, TBM_SETPOS, TRUE, slider_level);
    UpdateSliderStaticLabel(hDlg, hVolSlider);

    if (ses_settings.change_only_active_sessions)
        CheckDlgButton(hDlg, IDC_CHECK_APPLYONLYACTIVE, BST_CHECKED);
    else
        CheckDlgButton(hDlg, IDC_CHECK_APPLYONLYACTIVE, BST_UNCHECKED);

    std::wstring delay_wstring = L"0";
    if (ses_settings.vol_up_delay.count() != 0)
        delay_wstring = std::to_wstring(ses_settings.vol_up_delay.count());
    SetDlgItemText(hDlg, IDC_EDIT_DELAYTIME, delay_wstring.c_str());

    // Set tooltip help for controls
    SetControlTooltips(hDlg);
}

/*
    Takes values from dialog controls and updates VolumeOptions settings.
*/
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

    state = IsDlgButtonChecked(hDlg, IDC_CHECK_VOLUMEASPERCENTAGE);
    if (state == BST_CHECKED)
        ses_settings.treat_vol_as_percentage = true;
    else
        ses_settings.treat_vol_as_percentage = false;

    // Get volume level from slider value
    HWND hVolSlider = GetDlgItem(hDlg, IDC_SLIDER_VOLUMELEVEL);
    int value = static_cast<int>(SendMessage(hVolSlider, TBM_GETPOS, 0, 0));
    float vol_value = static_cast<float>(value) / 100;
    ses_settings.vol_reduction = vol_value;
    wprintf(L"Trackbar pos = %f \n", ses_settings.vol_reduction);

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

    // included list
    memset(process_names, 0, LIST_MAX_SIZE);
    GetDlgItemText(hDlg, IDC_EDIT_INCLUDEFILTER, process_names, LIST_MAX_SIZE);
    vo::parse_process_list(process_names, mon_settings.included_process);
    dwprintf(L"excluded process_names: %s\n\n", process_names);

    // excluded list
    memset(process_names, 0, LIST_MAX_SIZE);
    GetDlgItemText(hDlg, IDC_EDIT_EXCLUDEFILTER, process_names, LIST_MAX_SIZE);
    vo::parse_process_list(process_names, mon_settings.excluded_process);
    dwprintf(L"included process_names: %s\n\n", process_names);

}

INT_PTR CALLBACK VODialogProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
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

       // case IDC_CHECK_EXCLUDEOWNCLIENT:
           // return TRUE;

      //  case IDC_CHECK_APPLYONLYACTIVE:
       //     if (HIWORD(wParam) == BN_CLICKED)
      //      {
       //         dwprintf(L"BN_CLICKED\n");
       //     }
       //     return TRUE;

        case IDC_CHECK_VOLUMEASPERCENTAGE:
        {
            if (HIWORD(wParam) == BN_CLICKED)
            {
                UpdateSliderControls(hDlg);
            }
        }
            return TRUE;

       // case IDC_CHECK_EXCLUDEOWNPROCESS:
         //   return TRUE;

        case IDC_RADIO_EXCLUDEFILTER:
        {
            HWND include_edit = GetDlgItem(hDlg, IDC_EDIT_INCLUDEFILTER);
            HWND exclude_edit = GetDlgItem(hDlg, IDC_EDIT_EXCLUDEFILTER);
            EnableWindow(include_edit, FALSE);
            EnableWindow(exclude_edit, TRUE);
        }
            return TRUE;

        case IDC_RADIO_INCLUDEFILTER:
        {
            HWND include_edit = GetDlgItem(hDlg, IDC_EDIT_INCLUDEFILTER);
            HWND exclude_edit = GetDlgItem(hDlg, IDC_EDIT_EXCLUDEFILTER);
            EnableWindow(include_edit, TRUE);
            EnableWindow(exclude_edit, FALSE);

        }
            return TRUE;

        default:
            return FALSE;
        }
    }
    break;

    case WM_CTLCOLORSTATIC:
    {
        // Paint warning note shown when vol reduction is negative
        if ((HWND)lParam == GetDlgItem(hDlg, IDC_STATIC_NOTE_WARNING_VOL))
        {
            SetTextColor((HDC)wParam, RGB(255, 52, 52));
            SetBkMode((HDC)wParam, TRANSPARENT);

            return (INT_PTR)GetSysColorBrush(COLOR_MENU);
        }
    }
    break;

    case WM_VSCROLL:
    {
        switch (LOWORD(wParam))
        {
        case SB_ENDSCROLL:
        case SB_LEFT:
        case SB_RIGHT:
        case SB_LINELEFT:
        case SB_LINERIGHT:
        case SB_PAGELEFT:
        case SB_PAGERIGHT:
            dwprintf(L"what lo=%d hi=%d\n", LOWORD(wParam), HIWORD(wParam));
            break;
        case SB_THUMBPOSITION:
            dwprintf(L"pos level = %d\n", HIWORD(wParam));
            break;

        case SB_THUMBTRACK:
            dwprintf(L"track level = %d\n", HIWORD(wParam));
            break;

        default:
            dwprintf(L"no handled lo=%d hi=%d\n", LOWORD(wParam), HIWORD(wParam));
        }

        // NOTE: slider tooltips take too much cpu, windows..
        // HIWORD(wParam) doesnt track the value if user just clicks the slider, so.. get pos.
        UpdateSliderStaticLabel(hDlg, (HWND)lParam);

        return 0;
    }
    break;

    case  WM_INITDIALOG:
    {
        pvo = (vo::VolumeOptions*)lParam;
        vo_settings = pvo->get_current_settings();

        CenterWindow(hDlg);

        InitControlValues(hDlg, vo_settings);

        SaveOpenedDialog(hDlg);

        return TRUE;
    }
    break;

    case WM_CLOSE:
    {
#if VO_MODELESS_DIALOG_GUI
        DestroyWindow(hDlg);
#else
        EndDialog(hDlg, S_OK);
#endif
        return TRUE;
    }
    break;

    case WM_DESTROY:
        DeleteSavedDialog(hDlg);
        PostQuitMessage(0);
        return TRUE;

    } // end switch

    return FALSE;
}

void initTooltipClass(HWND hDlg)
{
    HINSTANCE hinst = GetModuleHandle(NULL);

    HWND hwndTip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        hDlg, NULL, hinst,
        NULL);

    SetWindowPos(hwndTip, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

int DialogThread(vo::VolumeOptions* pvo, void* vparent)
{
    HWND parent = static_cast<HWND>(vparent);

    int nCmdShow = SW_SHOWNORMAL;
    HINSTANCE hInst = GetCurrentModule();

    dprintf("Init Common Controls and InitIsolationAware\n");

    InitCommonControls();
    InitIsolationAware(hInst);

    CActCtxActivator ScopedContext(g_context);

#if VO_MODELESS_DIALOG_GUI
    HWND hDlg;
    MSG msg;
    BOOL getret;

    dprintf("Creating Modeless dialog box:\n");
    hDlg = CreateDialogParam(hInst, MAKEINTRESOURCE(IDD_VO_CONFIG_DIALOG), parent, VODialogProc, LPARAM(pvo));
    if (hDlg == NULL)
    {
        DWORD err = GetLastError();
        dprintf("CreateDialogParam error: %d\n", err); // TODO, boost::error_codes
        return err;
    }
    ShowWindow(hDlg, nCmdShow); // modeless dialog boxes must be show if not style defined to


    dprintf("Joining Modeless Dialog message loop... hDlg= %d\n", (int)hDlg);

    while (getret = GetMessage(&msg, 0, 0, 0))
    {
        if (getret == -1)
            break;

        if (!IsDialogMessage(hDlg, &msg)) 
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    dprintf("Exiting Modeless Dialog message loop with code GetMessage= %d, msg.wParam = %d\n",
        getret, (int)msg.wParam);

    return (int)msg.wParam;
#else
    dprintf("Creating Modal dialog box:\n");
    INT_PTR modalret = DialogBoxParam(hInst, MAKEINTRESOURCE(IDD_VO_CONFIG_DIALOG), parent, DialogProc, LPARAM(pvo));
    if (modalret != S_OK)
    {
        DWORD err = GetLastError();
        dprintf("DialogBoxParam error: %d\n", err); // TODO, boost::error_codes
        return err;
    }

    dprintf("Exiting Modal Dialog with code: %d\n",  (int)modalret);

    return (int)modalret;
#endif
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