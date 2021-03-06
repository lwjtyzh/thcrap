#include <thcrap.h>
#include <windows.h>
#include <windowsx.h>
#include "console.h"
#include "resource.h"
#include <string.h>
#include <CommCtrl.h>
#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <thread>

template<typename T>
using EventPtr = const std::shared_ptr<std::promise<T>>*;

#define APP_READQUEUE (WM_APP+0)
/* lparam = const std::shared_ptr<std::promise<char>>* */
#define APP_ASKYN (WM_APP+1)
/* wparam = length of output string */
/* lparam = const std::shared_ptr<std::promise<wchar_t*>>* */
#define APP_GETINPUT (WM_APP+2)
/* wparam = WM_CHAR */
#define APP_LISTCHAR (WM_APP+3)
#define APP_UPDATE (WM_APP+4)
#define APP_PREUPDATE (WM_APP+5)
/* lparam = const std::shared_ptr<std::promise<void>>* */
#define APP_PAUSE (WM_APP+6)
/* wparam = percent */
#define APP_PROGRESS (WM_APP+7)

/* Reads line queue */
#define Thcrap_ReadQueue(hwndDlg) ((void)::PostMessage((hwndDlg), APP_READQUEUE, 0, 0L))
#define Thcrap_Askyn(hwndDlg,a_promise) ((void)::PostMessage((hwndDlg), APP_ASKYN, 0, (LPARAM)(EventPtr<char>)(a_promise)))
/* Request input */
/* UI thread will signal g_event when user confirms input */
#define Thcrap_GetInput(hwndDlg,len,a_promise) ((void)::PostMessage((hwndDlg), APP_GETINPUT, (WPARAM)(DWORD)(len), (LPARAM)(EventPtr<wchar_t*>)(a_promise)))
/* Should be called after adding/appending bunch of lines */
#define Thcrap_Update(hwndDlg) ((void)::PostMessage((hwndDlg), APP_UPDATE, 0, 0L))
/* Should be called before adding lines*/
#define Thcrap_Preupdate(hwndDlg) ((void)::PostMessage((hwndDlg), APP_PREUPDATE, 0, 0L))
/* Pause */
#define Thcrap_Pause(hwndDlg,a_promise) ((void)::PostMessage((hwndDlg), APP_PAUSE, 0, (LPARAM)(EventPtr<void>)(a_promise)))
/* Updates progress bar */
#define Thcrap_Progress(hwndDlg, pc) ((void)::PostMessage((hwndDlg), APP_PROGRESS, (WPARAM)(DWORD)(pc), 0L))

enum LineType {
	LINE_ADD,
	LINE_APPEND,
	LINE_CLS,
	LINE_PENDING
};
struct LineEntry {
	LineType type;
	std::wstring content;
};
template<typename T>
class WaitForEvent {
	std::shared_ptr<std::promise<T>> promise;
	std::future<T> future;
public:
	WaitForEvent() {
		promise = std::make_shared<std::promise<T>>();
		future = promise->get_future();
	}
	EventPtr<T> getEvent() {
		return &promise;
	}
	T getResponse() {
		return future.get();
	}
};
template<typename T>
void SignalEvent(std::shared_ptr<std::promise<T>>& ptr, T val) {
	if (ptr) {
		ptr->set_value(val);
		ptr.reset();
	}
}
void SignalEvent(std::shared_ptr<std::promise<void>>& ptr) {
	if (ptr) {
		ptr->set_value();
		ptr.reset();
	}
}

static HWND g_hwnd = NULL;
static std::mutex g_mutex; // used for synchronizing the queue
static std::queue<LineEntry> g_queue;
static std::vector<std::wstring> q_responses;
static std::shared_ptr<std::promise<void>> g_exitguithreadevent;
static bool* CurrentMode = nullptr;
static void SetMode(HWND hwndDlg, bool* mode) {
	if (CurrentMode == mode) return;
	CurrentMode = mode;
	int items[] = { IDC_BUTTON1, IDC_EDIT1, IDC_PROGRESS1, IDC_STATIC1, IDC_BUTTON_YES, IDC_BUTTON_NO};
	for (int i = 0; i < _countof(items); i++) {
		HWND item = GetDlgItem(hwndDlg, items[i]);
		ShowWindow(item, mode[i] ? SW_SHOW : SW_HIDE);
		EnableWindow(item, mode[i] ? TRUE : FALSE);
	}
}
static bool InputMode[] = { true, true, false, false, false, false };
static bool PauseMode[] = { true, false, false, false, false, false };
static bool ProgressBarMode[] = { false, false, true, false, false, false};
static bool NoMode[] = { false, false, false, true, false, false};
static bool AskYnMode[] = { false, false, false, false, true, true};

WNDPROC origEditProc = NULL;
LRESULT CALLBACK EditProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
		SendMessage(GetParent(hwnd), WM_COMMAND, MAKELONG(IDC_BUTTON1, BN_CLICKED), (LPARAM)hwnd);
		return 0;
	}
	else if (uMsg == WM_GETDLGCODE && wParam == VK_RETURN) {
		// nescessary for control to recieve VK_RETURN
		return origEditProc(hwnd, uMsg, wParam, lParam) | DLGC_WANTALLKEYS;
	}
	else if (uMsg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) {
		// Switch focus to list on up/down arrows
		HWND list = GetDlgItem(GetParent(hwnd), IDC_LIST1);
		SetFocus(list);
		return 0;
	}
	else if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) && (wParam == VK_PRIOR || wParam == VK_NEXT)) {
		// Switch focus to list on up/down arrows
		HWND list = GetDlgItem(GetParent(hwnd), IDC_LIST1);
		SendMessage(list, uMsg, wParam, lParam);
		return 0;
	}
	return origEditProc(hwnd,uMsg,wParam,lParam);
}

WNDPROC origListProc = NULL;
LRESULT CALLBACK ListProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_KEYDOWN && wParam == VK_RETURN) {
		// if edit already has something in it, clicking on the button
		// otherwise doubleclick the list
		int len = GetWindowTextLength(GetDlgItem(GetParent(hwnd), IDC_EDIT1));
		SendMessage(GetParent(hwnd), WM_COMMAND, len ? MAKELONG(IDC_BUTTON1, BN_CLICKED) : MAKELONG(IDC_LIST1, LBN_DBLCLK), (LPARAM)hwnd);
		return 0;
	}
	else if (uMsg == WM_GETDLGCODE && wParam == VK_RETURN) {
		// nescessary for control to recieve VK_RETURN
		return origListProc(hwnd, uMsg, wParam, lParam) | DLGC_WANTALLKEYS;
	}
	else if (uMsg == WM_CHAR) {
		// let parent dialog process the keypresses from list
		SendMessage(GetParent(hwnd), APP_LISTCHAR, wParam, lParam);
		return 0;
	}
	return origListProc(hwnd, uMsg, wParam, lParam);
}

bool con_can_close = false;
INT_PTR CALLBACK DlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	static int input_len = 0;
	static int last_index = 0;
	static std::wstring pending = L"";
	static std::shared_ptr<std::promise<char>> promiseyn; // APP_ASKYN event
	static std::shared_ptr<std::promise<wchar_t*>> promise; // APP_GETINPUT event
	static std::shared_ptr<std::promise<void>> promisev; // APP_PAUSE event
	static HICON hIconSm = NULL, hIcon = NULL;
	switch (uMsg) {
	case WM_INITDIALOG: {
		g_hwnd = hwndDlg;

		SendMessage(GetDlgItem(hwndDlg, IDC_PROGRESS1), PBM_SETRANGE, 0, MAKELONG(0, 100));
		origEditProc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hwndDlg, IDC_EDIT1), GWLP_WNDPROC, (LONG)EditProc);
		origListProc = (WNDPROC)SetWindowLongPtr(GetDlgItem(hwndDlg, IDC_LIST1), GWLP_WNDPROC, (LONG)ListProc);

		// set icon
		hIconSm = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
		hIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
		if (hIconSm) {
			SendMessage(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
		}
		if (hIcon) {
			SendMessage(hwndDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
		}
		SetMode(hwndDlg, NoMode);

		RECT workarea, winrect;
		SystemParametersInfo(SPI_GETWORKAREA, 0, (PVOID)&workarea, 0);
		GetWindowRect(hwndDlg, &winrect);
		int width = winrect.right - winrect.left;
		MoveWindow(hwndDlg,
			workarea.left + ((workarea.right - workarea.left) / 2) - (width / 2),
			workarea.top,
			width,
			workarea.bottom - workarea.top,
			TRUE);

		(*(EventPtr<void>)lParam)->set_value();
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_BUTTON1: {
			if (CurrentMode == InputMode) {
				wchar_t* input_str = new wchar_t[input_len];
				GetDlgItemTextW(hwndDlg, IDC_EDIT1, input_str, input_len);
				SetDlgItemTextW(hwndDlg, IDC_EDIT1, L"");
				SetMode(hwndDlg, NoMode);
				SignalEvent(promise, input_str);
			}
			else if (CurrentMode == PauseMode) {
				SetMode(hwndDlg, NoMode);
				SignalEvent(promisev);
			}
			return TRUE;
		}
		case IDC_BUTTON_YES:
		case IDC_BUTTON_NO: {
			if (CurrentMode == AskYnMode) {
				SetMode(hwndDlg, NoMode);
				SignalEvent(promiseyn, LOWORD(wParam) == IDC_BUTTON_YES ? 'y' : 'n');
			}
			return TRUE;
		}
		case IDC_LIST1:
			switch (HIWORD(wParam)) {
			case LBN_DBLCLK: {
				int cur = ListBox_GetCurSel((HWND)lParam);
				if (CurrentMode == InputMode && cur != LB_ERR && (!q_responses[cur].empty() || cur == last_index)) {
					wchar_t* input_str = new wchar_t[q_responses[cur].length() + 1];
					wcscpy(input_str, q_responses[cur].c_str());
					SetDlgItemTextW(hwndDlg, IDC_EDIT1, L"");
					SetMode(hwndDlg, NoMode);
					SignalEvent(promise, input_str);
				}
				else if (CurrentMode == PauseMode) {
					SetMode(hwndDlg, NoMode);
					SignalEvent(promisev);
				}
				return TRUE;
			}
			}
		}
		return FALSE;
	case APP_LISTCHAR: {
		HWND edit = GetDlgItem(hwndDlg, IDC_EDIT1);
		if (CurrentMode == InputMode) {
			SetFocus(edit);
			SendMessage(edit, WM_CHAR, wParam, lParam);
		}
		else if (CurrentMode == AskYnMode) {
			char c = wctob(towlower(wParam));
			if (c == 'y' || c == 'n') {
				SetMode(hwndDlg, NoMode);
				SignalEvent(promiseyn, c);
			}
		}
		return TRUE;
	}
	case APP_READQUEUE: {
		HWND list = GetDlgItem(hwndDlg, IDC_LIST1);
		std::lock_guard<std::mutex> lock(g_mutex);
		while (!g_queue.empty()) {
			LineEntry& ent = g_queue.front();
			switch (ent.type) {
			case LINE_ADD:
				last_index = ListBox_AddString(list, ent.content.c_str());
				q_responses.push_back(pending);
				pending = L"";
				break;
			case LINE_APPEND: {
				int origlen = ListBox_GetTextLen(list, last_index);
				int len = origlen + ent.content.length() + 1;
				VLA(wchar_t, wstr, len);
				ListBox_GetText(list, last_index, wstr);
				wcscat_s(wstr, len, ent.content.c_str());
				ListBox_DeleteString(list, last_index);
				ListBox_InsertString(list, last_index, wstr);
				VLA_FREE(wstr);
				if (!pending.empty()) {
					q_responses[last_index] = pending;
					pending = L"";
				}
				break;
			}
			case LINE_CLS:
				ListBox_ResetContent(list);
				q_responses.clear();
				pending = L"";
				break;
			case LINE_PENDING:
				pending = ent.content;
				break;
			}
			g_queue.pop();
		}
		return TRUE;
	}
	case APP_GETINPUT:
		SetMode(hwndDlg, InputMode);
		input_len = wParam;
		promise = *(EventPtr<wchar_t*>)lParam;
		return TRUE;
	case APP_PAUSE:
		SetMode(hwndDlg, PauseMode);
		promisev = *(EventPtr<void>)lParam;
		return TRUE;
	case APP_ASKYN:
		SetMode(hwndDlg, AskYnMode);
		promiseyn = *(EventPtr<char>)lParam;
		return TRUE;
	case APP_PREUPDATE: {
		HWND list = GetDlgItem(hwndDlg, IDC_LIST1);
		SetWindowRedraw(list, FALSE);
		return TRUE;
	}
	case APP_UPDATE: {
		HWND list = GetDlgItem(hwndDlg, IDC_LIST1);
		//SendMessage(list, WM_VSCROLL, SB_BOTTOM, 0L);
		ListBox_SetCurSel(list, last_index);// Just scrolling doesn't work well
		SetWindowRedraw(list, TRUE);
		// this causes unnescessary flickering
		//RedrawWindow(list, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
		return TRUE;
	}
	
	case APP_PROGRESS: {
		HWND progress = GetDlgItem(hwndDlg, IDC_PROGRESS1);
		if (wParam == -1) {
			SetWindowLong(progress, GWL_STYLE, GetWindowLong(progress, GWL_STYLE) | PBS_MARQUEE);
			SendMessage(progress, PBM_SETMARQUEE, 1, 0L);
		}
		else {
			SendMessage(progress, PBM_SETMARQUEE, 0, 0L);
			SetWindowLong(progress, GWL_STYLE, GetWindowLong(progress, GWL_STYLE) & ~PBS_MARQUEE);
			SendMessage(progress, PBM_SETPOS, wParam, 0L);
		}

		SetMode(hwndDlg, ProgressBarMode);
		return TRUE;
	}
	case WM_SIZE: {
		RECT baseunits;
		baseunits.left = baseunits.right = 100;
		baseunits.top = baseunits.bottom = 100;
		MapDialogRect(hwndDlg, &baseunits);
		int basex = 4 * baseunits.right / 100;
		int basey = 8 * baseunits.bottom / 100;
		long origright = LOWORD(lParam) * 4 / basex;
		long origbottom = HIWORD(lParam) * 8 / basey;
		RECT rbutton1, rbutton2, rlist, redit, rwide;
		const int MARGIN = 7;
		const int PADDING = 2;
		const int BTN_WIDTH = 50;
		const int BTN_HEIGHT = 14;
		// |-----------wide----------|
		// |-----edit-------|        |
		// |       |--btn2--|--btn1--|

		// Button size
		rbutton1 = {
			origright - MARGIN - BTN_WIDTH,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN,
			origbottom - MARGIN };
		MapDialogRect(hwndDlg, &rbutton1);
		// Progress/staic size
		rwide = {
			MARGIN,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN,
			origbottom - MARGIN };
		MapDialogRect(hwndDlg, &rwide);
		// Edit size
		redit = {
			MARGIN,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN - BTN_WIDTH - PADDING,
			origbottom - MARGIN };
		MapDialogRect(hwndDlg, &redit);
		// Button2 size
		rbutton2 = {
			origright - MARGIN - BTN_WIDTH - PADDING - BTN_WIDTH,
			origbottom - MARGIN - BTN_HEIGHT,
			origright - MARGIN - BTN_WIDTH - PADDING,
			origbottom - MARGIN };
		MapDialogRect(hwndDlg, &rbutton2);
		// List size
		rlist = {
			MARGIN,
			MARGIN,
			origright - MARGIN,
			origbottom - MARGIN - BTN_HEIGHT - PADDING};
		MapDialogRect(hwndDlg, &rlist);
#define MoveToRect(hwnd,rect) MoveWindow((hwnd), (rect).left, (rect).top, (rect).right - (rect).left, (rect).bottom - (rect).top, TRUE)
		MoveToRect(GetDlgItem(hwndDlg, IDC_BUTTON1), rbutton1);
		MoveToRect(GetDlgItem(hwndDlg, IDC_EDIT1), redit);
		MoveToRect(GetDlgItem(hwndDlg, IDC_LIST1), rlist);
		MoveToRect(GetDlgItem(hwndDlg, IDC_PROGRESS1), rwide);
		MoveToRect(GetDlgItem(hwndDlg, IDC_STATIC1), rwide);
		MoveToRect(GetDlgItem(hwndDlg, IDC_BUTTON_YES), rbutton2);
		MoveToRect(GetDlgItem(hwndDlg, IDC_BUTTON_NO), rbutton1);
		InvalidateRect(hwndDlg, &rwide, FALSE); // needed because static doesn't repaint on move
		return TRUE;
	}
	case WM_CLOSE:
		if (con_can_close || MessageBoxW(hwndDlg, L"Patch configuration is not finished.\n\nQuit anyway?", L"Touhou Community Reliant Automatic Patcher", MB_YESNO | MB_ICONWARNING) == IDYES) {
			EndDialog(hwndDlg, 0);
		}
		return TRUE;
	case WM_NCDESTROY:
		g_hwnd = NULL;
		if (hIconSm) {
			DestroyIcon(hIconSm);
		}
		if (hIcon) {
			DestroyIcon(hIcon);
		}
		return TRUE;
	}
	return FALSE;
}
static bool needAppend = false;
static bool dontUpdate = false;
void log_windows(const char* text) {
	if (!g_hwnd) return;
	if (!dontUpdate) Thcrap_Preupdate(g_hwnd);

	int len = strlen(text) + 1;
	VLA(wchar_t, wtext, len);
	StringToUTF16(wtext, text, len);
	wchar_t *start = wtext, *end = wtext;
	while (end) {
		int mutexcond = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			end = wcschr(end, '\n');
			if (!end) end = wcschr(start, '\0');
			wchar_t c = *end++;
			if (c == '\n') {
				end[-1] = '\0'; // Replace newline with null
			}
			else if (c == '\0') {
				if (end != wtext && end == start) {
					break; // '\0' right after '\n'
				}
				end = NULL;
			}
			else continue;
			if (needAppend == true) {
				LineEntry le = { LINE_APPEND, start };
				g_queue.push(le);
				needAppend = false;
			}
			else {
				LineEntry le = { LINE_ADD, start };
				g_queue.push(le);
			}
			mutexcond = g_queue.size() > 10;
		}
		if (mutexcond) {
			Thcrap_ReadQueue(g_hwnd);
			if (dontUpdate) {
				Thcrap_Update(g_hwnd);
				Thcrap_Preupdate(g_hwnd);
			}
		}
		start = end;
	}
	VLA_FREE(wtext);
	if (!end) needAppend = true;

	if (!dontUpdate) {
		Thcrap_ReadQueue(g_hwnd);
		Thcrap_Update(g_hwnd);
	}
}
void log_nwindows(const char* text, size_t len) {
	VLA(char, ntext, len+1);
	memcpy(ntext, text, len);
	ntext[len] = '\0';
	log_windows(ntext);
	VLA_FREE(ntext);
}
/* --- code proudly stolen from thcrap/log.cpp --- */
#define VLA_VSPRINTF(str, va) \
	size_t str##_full_len = _vscprintf(str, va) + 1; \
	VLA(char, str##_full, str##_full_len); \
	/* vs*n*printf is not available in msvcrt.dll. Doesn't matter anyway. */ \
	vsprintf(str##_full, str, va);

void con_vprintf(const char *str, va_list va)
{
	if (str) {
		VLA_VSPRINTF(str, va);
		log_windows(str_full);
		//printf("%s", str_full);
		VLA_FREE(str_full);
	}
}

void con_printf(const char *str, ...)
{
	if (str) {
		va_list va;
		va_start(va, str);
		con_vprintf(str, va);
		va_end(va);
	}
}
/* ------------------- */
void con_clickable(const char* response) {
	int len = strlen(response) + 1;
	VLA(wchar_t, wresponse, len);
	StringToUTF16(wresponse, response, len);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		LineEntry le = { LINE_PENDING,  wresponse };
		g_queue.push(le);
	}
	VLA_FREE(wresponse);
}

void con_clickable(int response) {
	size_t response_full_len = _scprintf("%d", response) + 1;
	VLA(char, response_full, response_full_len);
	sprintf(response_full, "%d", response);
	con_clickable(response_full);
}
void console_init() {
	INITCOMMONCONTROLSEX iccex;
	iccex.dwSize = sizeof(iccex);
	iccex.dwICC = ICC_PROGRESS_CLASS;
	InitCommonControlsEx(&iccex);

	log_set_hook(log_windows, log_nwindows);

	WaitForEvent<void> e;
	std::thread([](LPARAM lParam) {
		DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_DIALOG1), NULL, DlgProc, lParam);
		log_set_hook(NULL, NULL);
		if (!g_exitguithreadevent) {
			exit(0); // Exit the entire process when exiting this thread
		}
		else {
			SignalEvent(g_exitguithreadevent);
		}
	}, (LPARAM)e.getEvent()).detach();
	e.getResponse();
}
char* console_read(char *str, int n) {
	dontUpdate = false;
	Thcrap_ReadQueue(g_hwnd);
	Thcrap_Update(g_hwnd);
	WaitForEvent<wchar_t*> e;
	Thcrap_GetInput(g_hwnd, n, e.getEvent());
	wchar_t* input = e.getResponse();
	StringToUTF8(str, input, n);
	delete[] input;
	needAppend = false; // gotta insert that newline
	return str;
}
void cls(SHORT top) {
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		LineEntry le = { LINE_CLS, L"" };
		g_queue.push(le);
		Thcrap_ReadQueue(g_hwnd);
		needAppend = false;
	}
	if (dontUpdate) {
		Thcrap_Update(g_hwnd);
		Thcrap_Preupdate(g_hwnd);
	}
}
void pause(void) {
	dontUpdate = false;
	con_printf("Press ENTER to continue . . . "); // this will ReadQueue and Update for us
	needAppend = false;
	WaitForEvent<void> e;
	Thcrap_Pause(g_hwnd, e.getEvent());
	e.getResponse();
}
void console_prepare_prompt(void) {
	Thcrap_Preupdate(g_hwnd);
	dontUpdate = true;
}
void console_print_percent(int pc) {
	Thcrap_Progress(g_hwnd, pc);
}
char console_ask_yn(const char* what) {
	dontUpdate = false;
	log_windows(what);
	needAppend = false;

	WaitForEvent<char> e;
	Thcrap_Askyn(g_hwnd, e.getEvent());
	return e.getResponse();
}
void con_end(void) {
	WaitForEvent<void> e;
	g_exitguithreadevent = *e.getEvent();
	SendMessage(g_hwnd, WM_CLOSE, 0, 0L);
	e.getResponse();
}
HWND con_hwnd(void) {
	return g_hwnd;
}
int console_width(void) {
	return 80;
}
