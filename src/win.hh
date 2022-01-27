#include "common.hh"
#include "win64/WebView2.h"

#include <signal.h>
#include <future>
#include <chrono>
#include <shobjidl.h>

#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"version.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"WebView2LoaderStatic.lib")

// for dark mode...
#include <uxtheme.h>
#include <dwmapi.h>
#include <wingdi.h>

#pragma comment(lib,"UxTheme.lib")
#pragma comment(lib,"Dwmapi.lib")
#pragma comment(lib,"Gdi32.lib")

inline void alert (const std::wstring &ws) {
  MessageBoxA(nullptr, Opkit::WStringToString(ws).c_str(), _TEXT("Alert"), MB_OK | MB_ICONSTOP);
}

inline void alert (const std::string &s) {
  MessageBoxA(nullptr, s.c_str(), _TEXT("Alert"), MB_OK | MB_ICONSTOP);
}

inline void alert (const char* s) {
  MessageBoxA(nullptr, s, _TEXT("Alert"), MB_OK | MB_ICONSTOP);
}

namespace Opkit {
  using IEnvHandler = ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
  using IConHandler = ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
  using INavHandler = ICoreWebView2NavigationCompletedEventHandler;
  using IRecHandler = ICoreWebView2WebMessageReceivedEventHandler;
  using IArgs = ICoreWebView2WebMessageReceivedEventArgs;

  class App: public IApp {
    public:
      MSG msg;
      _In_ HINSTANCE hInstance;
      DWORD mainThread = GetCurrentThreadId();

      static std::atomic<bool> isReady;

      App(void* h);
      int run();
      void exit();
      void kill();
      void restart();
      void dispatch(std::function<void()>);
      std::string getCwd(const std::string&);
  };

  std::atomic<bool> App::isReady {false};

	// constexpr COLORREF darkBkColor = 0x383838;
	// constexpr COLORREF darkTextColor = 0xFFFFFF;
	// static HBRUSH hbrBkgnd = nullptr;

  enum WINDOWCOMPOSITIONATTRIB {
    WCA_UNDEFINED = 0,
    WCA_NCRENDERING_ENABLED = 1,
    WCA_NCRENDERING_POLICY = 2,
    WCA_TRANSITIONS_FORCEDISABLED = 3,
    WCA_ALLOW_NCPAINT = 4,
    WCA_CAPTION_BUTTON_BOUNDS = 5,
    WCA_NONCLIENT_RTL_LAYOUT = 6,
    WCA_FORCE_ICONIC_REPRESENTATION = 7,
    WCA_EXTENDED_FRAME_BOUNDS = 8,
    WCA_HAS_ICONIC_BITMAP = 9,
    WCA_THEME_ATTRIBUTES = 10,
    WCA_NCRENDERING_EXILED = 11,
    WCA_NCADORNMENTINFO = 12,
    WCA_EXCLUDED_FROM_LIVEPREVIEW = 13,
    WCA_VIDEO_OVERLAY_ACTIVE = 14,
    WCA_FORCE_ACTIVEWINDOW_APPEARANCE = 15,
    WCA_DISALLOW_PEEK = 16,
    WCA_CLOAK = 17,
    WCA_CLOAKED = 18,
    WCA_ACCENT_POLICY = 19,
    WCA_FREEZE_REPRESENTATION = 20,
    WCA_EVER_UNCLOAKED = 21,
    WCA_VISUAL_OWNER = 22,
    WCA_HOLOGRAPHIC = 23,
    WCA_EXCLUDED_FROM_DDA = 24,
    WCA_PASSIVEUPDATEMODE = 25,
    WCA_USEDARKMODECOLORS = 26,
    WCA_LAST = 27
  };

  struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    PVOID pvData;
    SIZE_T cbData;
  };

  using RefreshImmersiveColorPolicyState = VOID(WINAPI*)();
  using ShouldSystemUseDarkMode = BOOL(WINAPI*)();
  using AllowDarkModeForApp = BOOL(WINAPI*)(BOOL allow);
  using SetWindowCompositionAttribute = BOOL(WINAPI *)(HWND hWnd, WINDOWCOMPOSITIONATTRIBDATA*);

  static RefreshImmersiveColorPolicyState refreshImmersiveColorPolicyState = nullptr;
  static ShouldSystemUseDarkMode shouldSystemUseDarkMode = nullptr;
  static AllowDarkModeForApp allowDarkModeForApp = nullptr;
  static SetWindowCompositionAttribute setWindowCompositionAttribute = nullptr;

  static auto bgBrush = CreateSolidBrush(RGB(0, 0, 0));
  FILE* console;

  class Window : public IWindow {
    HWND window;
    DWORD mainThread = GetCurrentThreadId();

    public:
      Window(App&, WindowOptions);
      static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
      ICoreWebView2 *webview = nullptr;
      ICoreWebView2Controller *controller = nullptr;
      bool webviewFailed = false;

      App app;
      WindowOptions opts;
      std::map<int, std::string> menuMap;

      void resize (HWND window);
      POINT m_minsz = POINT {0, 0};
      POINT m_maxsz = POINT {0, 0};
      HMENU systemMenu;

      void about();
      void eval(const std::string&);
      void show(const std::string&);
      void hide(const std::string&);
      void exit();
      void kill();
      void close();
      void navigate(const std::string&, const std::string&);
      void setSize(const std::string&, int, int, int);
      void setTitle(const std::string&, const std::string&);
      void setContextMenu(const std::string&, const std::string&);
      void closeContextMenu(const std::string&);
      void closeContextMenu();
      void setSystemMenu(const std::string&, const std::string&);
      void openDialog(
        const std::string&,
        bool,
        bool,
        bool,
        bool,
        const std::string&,
        const std::string&
      );
      int openExternal(const std::string&);
      ScreenSize getScreenSize();
  };

  App::App(void* h): hInstance((_In_ HINSTANCE) h) {
    #if DEBUG == 1
      AllocConsole();
      freopen_s(&console, "CONOUT$", "w", stdout);
    #endif

    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

    setWindowCompositionAttribute = reinterpret_cast<SetWindowCompositionAttribute>(GetProcAddress(
      GetModuleHandleW(L"user32.dll"),
      "SetWindowCompositionAttribute")
    );

    if (hUxtheme) {
      refreshImmersiveColorPolicyState =
        (RefreshImmersiveColorPolicyState) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(104));

      shouldSystemUseDarkMode =
        (ShouldSystemUseDarkMode) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(138));

      allowDarkModeForApp =
        (AllowDarkModeForApp) GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
    }

    allowDarkModeForApp(shouldSystemUseDarkMode());
    refreshImmersiveColorPolicyState();

    // this fixes bad default quality DPI.
    SetProcessDPIAware();

    auto iconPath = fs::path { getCwd("") / fs::path { "index.ico" } };

    HICON icon = (HICON) LoadImageA(
      NULL,
      pathToString(iconPath).c_str(),
      IMAGE_ICON,
      GetSystemMetrics(SM_CXSMICON),
      GetSystemMetrics(SM_CXSMICON),
      LR_LOADFROMFILE
    );

    auto *szWindowClass = L"DesktopApp";
    auto *szTitle = L"Opkit";
    WNDCLASSEX wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = bgBrush;
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = TEXT("DesktopApp");
    wcex.hIconSm = icon; // ico doesn't auto scale, needs 16x16 icon lol fuck you bill
    wcex.hIcon = icon;
    wcex.lpfnWndProc = Window::WndProc;

    if (!RegisterClassEx(&wcex)) {
      alert("Application could not launch, possible missing resources.");
    }
  };

  Window::Window (App& app, WindowOptions opts) : app(app), opts(opts) {
    window = CreateWindow(
      TEXT("DesktopApp"), TEXT("Opkit"),
      WS_OVERLAPPEDWINDOW,
      100000,
      100000,
      1024, 780,
      NULL, NULL,
      app.hInstance, NULL
    );

    /* BOOL mode = FALSE;

    if (shouldSystemUseDarkMode()) {
      allowDarkModeForApp(true);
      refreshImmersiveColorPolicyState();
      mode = TRUE;
      // SetWindowTheme(window, L"DarkMode_Explorer", NULL);
    }

		WINDOWCOMPOSITIONATTRIBDATA data = {
      WCA_USEDARKMODECOLORS,
      &mode,
      sizeof(mode)
    };

		setWindowCompositionAttribute(window, &data);
    SetPropW(window, L"UseImmersiveDarkModeColors", (HANDLE)(LONG_PTR) mode);
    // SetPropW(window, L"UseImmersiveDarkModeColors", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(mode)));

    // SetWindowTheme(window, L"Explorer", nullptr);
    // DwmSetWindowAttribute(window, 19, &mode, sizeof(long));
    /*/
    UpdateWindow(window);
    ShowWindow(window, SW_SHOW);
    SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR) this);

    std::string preload(
      "window.external = {\n"
      "  invoke: arg => window.chrome.webview.postMessage(arg)\n"
      "};\n"
      "" + createPreload(opts) + "\n"
    );

    wchar_t modulefile[MAX_PATH];
    GetModuleFileNameW(NULL, modulefile, MAX_PATH);
    auto file = (fs::path { modulefile }).filename();
    auto filename = StringToWString(pathToString(file));
    auto path = StringToWString(getEnv("APPDATA"));

    auto res = CreateCoreWebView2EnvironmentWithOptions(
      nullptr,
      (path + L"/" + filename).c_str(),
      nullptr,
      Microsoft::WRL::Callback<IEnvHandler>(
        [&, preload](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
          env->CreateCoreWebView2Controller(
            window,
            Microsoft::WRL::Callback<IConHandler>(
              [&, preload](HRESULT result, ICoreWebView2Controller* c) -> HRESULT {
                hide("");

                if (c != nullptr) {
                  controller = c;
                  controller->get_CoreWebView2(&webview);

                  RECT bounds;
                  GetClientRect(window, &bounds);
                  controller->put_Bounds(bounds);
                  controller->AddRef();
                }

                ICoreWebView2Settings* Settings;
                webview->get_Settings(&Settings);
                Settings->put_IsScriptEnabled(TRUE);
                Settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                Settings->put_IsWebMessageEnabled(TRUE);
                Settings->put_IsStatusBarEnabled(FALSE);
                Settings->put_AreDevToolsEnabled(opts.debug == 1);
                Settings->put_AreDefaultContextMenusEnabled(opts.debug == 1);
                Settings->put_IsBuiltInErrorPageEnabled(FALSE);
                Settings->put_IsZoomControlEnabled(FALSE);

                auto settings3 = (ICoreWebView2Settings3*) Settings;
                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);

                auto settings6 = (ICoreWebView2Settings6*) Settings;
                settings6->put_IsPinchZoomEnabled(FALSE);
                settings6->put_IsSwipeNavigationEnabled(FALSE);

                app.isReady = true;

                EventRegistrationToken tokenNavigation;

                webview->add_NavigationStarting(
                  Callback<ICoreWebView2NavigationStartingEventHandler>(
                    [&](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *e) {
                      PWSTR uri;
                      e->get_Uri(&uri);
                      std::string url(WStringToString(uri));

                      if (url.find("file://") != 0) {
                        e->put_Cancel(true);
                      }

                      CoTaskMemFree(uri);
                      return S_OK;
                    }
                  ).Get(),
                  &tokenNavigation
                );

                webview->AddScriptToExecuteOnDocumentCreated(
                  StringToWString(preload).c_str(),
                  Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                    [&](HRESULT error, PCWSTR id) -> HRESULT {
                      return S_OK;
                    }
                  ).Get()
                );

                EventRegistrationToken tokenMessage;

                webview->add_WebMessageReceived(
                  Callback<IRecHandler>([&](ICoreWebView2* webview, IArgs* args) -> HRESULT {
                    LPWSTR messageRaw;
                    args->TryGetWebMessageAsString(&messageRaw);

                    if (onMessage != nullptr) {
                      std::wstring message(messageRaw);
                      onMessage(WStringToString(message));
                    }

                    CoTaskMemFree(messageRaw);
                    return S_OK;
                  }).Get(),
                  &tokenMessage
                );

                return S_OK;
              }
            ).Get()
          );

          return S_OK;
        }
      ).Get()
    );

    if (!SUCCEEDED(res)) {
      webviewFailed = true;
      // alert("Unable to create webview");
    }
  }

  int App::run () {
    MSG msg;
    BOOL res = GetMessage(&msg, nullptr, 0, 0);

    if (msg.hwnd) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }

    if (msg.message == WM_APP) {
      auto f = (std::function<void()> *)(msg.lParam);
      (*f)();
      delete f;
    }

    if (msg.message == WM_QUIT) {
      if (shouldExit) return 1;
    }

    return 0;
  }

  void App::kill () {
    shouldExit = true;
    PostQuitMessage(0);
  }

  void App::restart () {
    char szFileName[MAX_PATH] = "";
    GetModuleFileName(NULL, szFileName, MAX_PATH);

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    CreateProcess(NULL, szFileName, NULL, NULL, NULL, NULL, NULL, NULL, &si, &pi);

    std::exit(0);

    // WCHAR wsCommandLine[RESTART_MAX_CMD_LINE];
    // HRESULT hr = S_OK;             // not if registering for recovery and restart fails.

    // hr = RegisterApplicationRestart(wsCommandLine, RESTART_NO_PATCH | RESTART_NO_REBOOT);
    // if (FAILED(hr)) {
    //   alert("Failed to restart app");
    // }
  }

  void App::dispatch (std::function<void()> cb) {
    auto future = std::async(std::launch::async, [&] {
      while (!this->isReady) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }

      PostThreadMessage(
        mainThread,
        WM_APP,
        0,
        (LPARAM) new std::function<void()>(cb)
      );
    });

    future.get();
  }

  std::string App::getCwd (const std::string& _) {
    wchar_t filename[MAX_PATH];
    GetModuleFileNameW(NULL, filename, MAX_PATH);
    auto path = fs::path { filename }.remove_filename();
    return pathToString(path);
  }

  ScreenSize Window::getScreenSize () {
    return ScreenSize {
      .height = GetSystemMetrics(SM_CYFULLSCREEN),
      .width = GetSystemMetrics(SM_CXFULLSCREEN)
    };
  }

  void Window::about () {
    auto text = std::string(
      appData["title"] + " " +
      appData["version"] + "\n" +
      appData["copyRight"]
    );

    MSGBOXPARAMS mbp;
    mbp.cbSize = sizeof(MSGBOXPARAMS);
    mbp.hwndOwner = window;
    mbp.hInstance = app.hInstance;
    mbp.lpszText = text.c_str();
    mbp.lpszCaption = appData["title"].c_str();
    mbp.dwStyle = MB_USERICON;
    mbp.dwLanguageId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    mbp.lpfnMsgBoxCallback = NULL;
    mbp.dwContextHelpId = 0;

    MessageBoxIndirect(&mbp);
  }

  void Window::kill () {
    if (this->controller != nullptr) this->controller->Close();
    if (this->window != nullptr) DestroyWindow(this->window);
  }

  void Window::exit () {
    if (this->onExit != nullptr) this->onExit();
  }

  void Window::close () {
    if (opts.canExit) {
      this->exit();
    } else {
      DestroyWindow(window);
    }
  }

  void Window::show (const std::string& seq) {
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    RECT r, r1;
    GetWindowRect(window, &r);
    GetWindowRect(GetDesktopWindow(), &r1);

    MoveWindow(window, (
      (r1.right-r1.left) - (r.right-r.left)) / 2,
      ((r1.bottom-r1.top) - (r.bottom-r.top)) / 2,
      (r.right-r.left),
      (r.bottom-r.top),
      0
    );

    if (seq.size() > 0) {
      auto index = std::to_string(this->opts.index);
      this->onMessage(resolveToMainProcess(seq, "0", index));
    }
  }

  void Window::hide (const std::string& seq) {
    ShowWindow(window, SW_HIDE);
    UpdateWindow(window);
    this->eval(emitToRenderProcess("windowHide", "{}"));

    if (seq.size() > 0) {
      auto index = std::to_string(this->opts.index);
      this->onMessage(resolveToMainProcess(seq, "0", index));
    }
  }

  void Window::resize (HWND window) {
    if (controller == nullptr) {
      return;
    }

    RECT bounds;
    GetClientRect(window, &bounds);
    controller->put_Bounds(bounds);
  }

  void Window::eval (const std::string& s) {
    if (webview == nullptr) {
      return;
    }

    webview->ExecuteScript(
      StringToWString(s).c_str(),
      nullptr
    );
  }

  void Window::navigate (const std::string& seq, const std::string& value) {
    EventRegistrationToken token;
    auto index = std::to_string(this->opts.index);

    webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [&, seq, index](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
          std::string state = "1";

          BOOL success;
          args->get_IsSuccess(&success);

          if (success) {
            state = "0";
          }

          this->onMessage(resolveToMainProcess(seq, state, index));
          webview->remove_NavigationCompleted(token);

          return S_OK;
        })
      .Get(),
      &token
    );

    webview->Navigate(StringToWString(value).c_str());
  }

  void Window::setTitle (const std::string& seq, const std::string& title) {
    SetWindowText(window, title.c_str());

    if (onMessage != nullptr) {
      std::string state = "0"; // can this call actually fail?
      auto index = std::to_string(this->opts.index);

      this->onMessage(resolveToMainProcess(seq, state, index));
    }
  }

  void Window::setSize (const std::string& seq, int width, int height, int hints) {
    auto style = GetWindowLong(window, GWL_STYLE);

    if (hints == WINDOW_HINT_FIXED) {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    } else {
      style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    }

    SetWindowLong(window, GWL_STYLE, style);

    if (hints == WINDOW_HINT_MAX) {
      m_maxsz.x = width;
      m_maxsz.y = height;
    } else if (hints == WINDOW_HINT_MIN) {
      m_minsz.x = width;
      m_minsz.y = height;
    } else {
      RECT r;
      r.left = r.top = 0;
      r.right = width;
      r.bottom = height;

      AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);

      SetWindowPos(
        window,
        NULL,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED
      );

      resize(window);
    }

    if (seq.size() > 0) {
      auto index = std::to_string(this->opts.index);
      this->onMessage(resolveToMainProcess(seq, "0", index));
    }
  }

  void Window::setSystemMenu (const std::string& seq, const std::string& value) {
    std::string menu = value;

    HMENU hMenubar = GetMenu(window);

    // deserialize the menu
    menu = replace(menu, "%%", "\n");

    // split on ;
    auto menus = split(menu, ';');
    int itemId = 0;

    for (auto m : menus) {
      auto menu = split(m, '\n');
      auto line = trim(menu[0]);
      if (line.empty()) continue;
      auto menuTitle = split(line, ':')[0];

      HMENU hMenu = CreateMenu();

      for (int i = 1; i < menu.size(); i++) {
        auto line = trim(menu[i]);
        if (line.size() == 0) continue;

        if (line.empty()) {
          continue;
        }

        if (line.find("---") != -1) {
          AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
          continue;
        }

        auto parts = split(line, ':');
        auto title = parts[0];
        int mask = 0;
        std::string key = "";

        auto accelerators = split(parts[1], '+');
        auto accl = std::string("");

        key = trim(parts[1]) == "_" ? "" : trim(accelerators[0]);

        if (key.size() > 0) {
          bool isShift = std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ").find(key) != -1;
          accl = key;

          if (accelerators.size() > 1) {
            accl = std::string(trim(accelerators[1]) + "+" + key);
            accl = replace(accl, "CommandOrControl", "Ctrl");
            accl = replace(accl, "Command", "Ctrl");
            accl = replace(accl, "Control", "Ctrl");
          }

          if (isShift) {
            accl = std::string("Shift+" + accl);
          }
        }

        auto display = std::string(title + "\t" + accl);
        AppendMenuA(hMenu, MF_STRING, itemId, display.c_str());
        menuMap[itemId] = std::string(title + "\t" + menuTitle);
        itemId++;
      }

      AppendMenuA(hMenubar, MF_POPUP, (UINT_PTR) hMenu, menuTitle.c_str());
    }

    MENUINFO Info;
    Info.cbSize = sizeof(Info);
    Info.fMask = MIM_BACKGROUND; // | MFT_OWNERDRAW;
    Info.hbrBack = bgBrush;
    SetMenuInfo(hMenubar, &Info);

    if (seq.size() > 0) {
      auto index = std::to_string(this->opts.index);
      this->onMessage(resolveToMainProcess(seq, "0", index));
    }
  }

  void Window::closeContextMenu() {
    // @TODO(jwerle)
  }

  void Window::closeContextMenu(const std::string &seq) {
    // @TODO(jwerle)
  }

  void Window::setContextMenu (const std::string& seq, const std::string& value) {
    HMENU hPopupMenu = CreatePopupMenu();

    auto menuItems = split(value, '_');
    int index = 1;
    std::vector<std::string> lookup;
    lookup.push_back("");

    for (auto item : menuItems) {
      auto pair = split(trim(item), ':');
      auto key = std::string("");

      if (pair.size() > 1) {
        key = pair[1];
      }

      if (pair[0].find("---") != -1) {
        InsertMenu(hPopupMenu, 0, MF_SEPARATOR, 0, NULL);
      } else {
        lookup.push_back(pair[0]);
        InsertMenu(hPopupMenu, 0, MF_BYPOSITION | MF_STRING, index++, pair[0].c_str());
      }
    }

    SetForegroundWindow(window);

    POINT p;
    GetCursorPos(&p);

    auto selection = TrackPopupMenu(
      hPopupMenu,
      TPM_RETURNCMD | TPM_NONOTIFY,
      p.x,
      p.y,
      0,
      window,
      nullptr
    );

    DestroyMenu(hPopupMenu);
    if (selection == 0) return;
    this->eval(resolveMenuSelection(seq, lookup.at(selection), "contextMenu"));
  }

  int Window::openExternal (const std::string& url) {
    ShellExecute(nullptr, "Open", url .c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // TODO how to detect success here. do we care?
    return 0;
  }

  void Window::openDialog (
      const std::string& seq,
      bool isSave,
      bool allowDirs,
      bool allowFiles,
      bool allowMultiple,
      const std::string& defaultPath,
      const std::string& title
    ) {

    IFileOpenDialog * pfd;

    HRESULT hr = CoInitializeEx(
      NULL,
      COINIT_APARTMENTTHREADED |
      COINIT_DISABLE_OLE1DDE
    );

    if (FAILED(hr)) return;

    if (isSave) {
      hr = CoCreateInstance(
        CLSID_FileSaveDialog,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd)
      );

      hr = pfd->Show(NULL);
      if (FAILED(hr)) return;

    } else {
      hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd)
      );

      DWORD dwOptions;
      hr = pfd->GetOptions(&dwOptions);
      if (FAILED(hr)) return;

      if (allowDirs == true && allowFiles == false) {
        hr = pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        if (FAILED(hr)) return;
      }

      if ((!isSave || (!isSave && allowDirs)) && allowMultiple) {
        hr = pfd->SetOptions(dwOptions | FOS_ALLOWMULTISELECT);
        if (FAILED(hr)) return;
      }

      hr = pfd->Show(NULL);
      if (FAILED(hr)) return;

      IShellItemArray *results;
      hr = pfd->GetResults(&results);
      if (FAILED(hr)) return;

      DWORD count;
      results->GetCount(&count);

      std::vector<std::string> paths;

      for (DWORD i = 0; i < count; i++) {
        IShellItem *path;
        LPWSTR buf;
        results->GetItemAt(i, &path);
        path->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &buf);
        paths.push_back(WStringToString(std::wstring(buf)));
        path->Release();
        CoTaskMemFree(buf);
      }

      std::string result = "";

      for (size_t i = 0, i_end = paths.size(); i < i_end; ++i) {
        result += (i ? "\\n" : "");
        std::replace(paths[i].begin(), paths[i].end(), '\\', '/');
        result += paths[i];
      }

      auto wrapped =  std::string("\"" + result + "\"");
      this->eval(resolveToRenderProcess(seq, "0", encodeURIComponent(wrapped)));

      results->Release();
    }

    if (FAILED(hr)) return;

    pfd->Release();
    CoUninitialize();
  }

  LRESULT CALLBACK Window::WndProc(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    Window* w = reinterpret_cast<Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
      case WM_SIZE: {
        if (w == nullptr || w->webview == nullptr) {
          break;
        }

        RECT bounds;
        GetClientRect(hWnd, &bounds);
        w->controller->put_Bounds(bounds);
        break;
      }

      case WM_COMMAND: {
        if (w == nullptr) {
          break;
        }

        std::string meta(w->menuMap[wParam]);
        auto parts = split(meta, '\t');

        if (parts.size() > 1) {
          auto title = parts[0];
          auto parent = parts[1];

          if (std::string(title).find("About") == 0) {
            w->about();
            break;
          }

          if (std::string(title).find("Quit") == 0) {
            w->exit();
            break;
          }

          w->eval(resolveMenuSelection("0", title, parent));
        }

        break;
      }

      /* case WM_THEMECHANGED: {
        // alert("theme changed");
        if (::IsThemeActive())
          SetWindowLong(hWnd, GWL_EXSTYLE, GetWindowLong(hWnd, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        else
          SetWindowLong(hWnd, GWL_EXSTYLE, GetWindowLong(hWnd, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);
        return 0;
      } */

      /* case WM_CTLCOLORDLG:
      case WM_CTLCOLORSTATIC: {
        if (shouldSystemUseDarkMode()) {
          // HDC hdc = reinterpret_cast<HDC>(wParam);
          // SetTextColor(hdc, darkTextColor);
          // static auto testBrush = CreateSolidBrush(RGB(0, 0, 0));
          // SetBkColor(hdc, RGB(0, 0, 0));

          // if (!hbrBkgnd) {
          // hbrBkgnd = CreateSolidBrush(RGB(0, 0, 0));
          // }

          // return reinterpret_cast<INT_PTR>(bgBrush);
        }

        // static auto testBrush = CreateSolidBrush(RGB(0, 0, 0));
        // return (INT_PTR)(testBrush);
        break;
      } */

      // case WM_ERASEBKGND: {
      //  return 1;
      // }

      case WM_SETTINGCHANGE: {
        char* s = (char *) lParam;
        std::string name(s);

        if (name.find("ImmersiveColorSet") != -1) {
          BOOL darkMode = shouldSystemUseDarkMode();

          allowDarkModeForApp(darkMode);

          if (darkMode) {
            SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL);

            int aElements[4] = { COLOR_WINDOW, COLOR_ACTIVECAPTION, COLOR_MENUBAR, COLOR_MENU };
            DWORD aNewColors[4] = { RGB(32, 32, 32), RGB(32, 32, 32), RGB(32, 32, 32), RGB(32, 32, 32) };
            SetSysColors(4, aElements, aNewColors);
          } else {
            SetWindowTheme(hWnd, L"Explorer", nullptr);
            int aElements[4] = { COLOR_WINDOW, COLOR_ACTIVECAPTION, COLOR_MENUBAR, COLOR_MENU };
            DWORD aNewColors[4] = { RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255) };
            SetSysColors(4, aElements, aNewColors);
          }

          refreshImmersiveColorPolicyState();

          //SetPropW(hWnd, L"UseImmersiveDarkModeColors", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(mode)));
          /* SetPropW(hWnd, L"UseImmersiveDarkModeColors", (HANDLE)(LONG_PTR)mode);

          WINDOWCOMPOSITIONATTRIBDATA data = {
            WCA_USEDARKMODECOLORS,
            &mode,
            sizeof(mode)
          };

          setWindowCompositionAttribute(hWnd, &data);

          // DwmSetWindowAttribute(hWnd, 19, &mode, sizeof(long));
          // UpdateWindow(hWnd);

          BOOL mode = FALSE;

          if (shouldSystemUseDarkMode && shouldSystemUseDarkMode()) {
            // if (allowDarkModeForApp) allowDarkModeForApp(true);
            //if (refreshImmersiveColorPolicyState) refreshImmersiveColorPolicyState();
            // SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL);
            mode = TRUE;

            // int aElements[4] = { COLOR_WINDOW, COLOR_ACTIVECAPTION, COLOR_MENUBAR, COLOR_MENU };
            // DWORD aNewColors[4] = { RGB(32, 32, 32), RGB(255, 255, 255), RGB(32, 32, 32), RGB(32, 32, 32) };
            // SetSysColors(4, aElements, aNewColors);
          } else {
            // int aElements[4] = { COLOR_WINDOW, COLOR_ACTIVECAPTION, COLOR_MENUBAR, COLOR_MENU };
            // DWORD aNewColors[4] = { RGB(255, 255, 255), RGB(0, 0, 0), RGB(255, 255, 255), RGB(255, 255, 255) };
            // SetSysColors(4, aElements, aNewColors);
          }

          // SetWindowTheme(hWnd, L"Explorer", NULL);

          // SetPropW(hWnd, L"UseImmersiveDarkModeColors", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(mode)));
          // DwmSetWindowAttribute(hWnd, 20, &mode, sizeof(long));
          // WINDOWCOMPOSITIONATTRIBDATA data = { WCA_USEDARKMODECOLORS, &mode, sizeof(mode) };
          // setWindowCompositionAttribute(hWnd, &data);
          // UpdateWindow(hWnd);

          // DrawMenuBar(hWnd);
          //
          // HMENU hMenubar = GetMenu(hWnd);
          // SetMenu(hWnd, hMenubar);
          */
        }
        break;
      }

      case WM_CREATE: {
        if (shouldSystemUseDarkMode()) {
          SetWindowTheme(hWnd, L"DarkMode_Explorer", NULL);
        } else {
          SetWindowTheme(hWnd, L"Explorer", NULL);
          // int aElements[4] = { COLOR_WINDOW, COLOR_ACTIVECAPTION, COLOR_MENUBAR, COLOR_MENU };
          // DWORD aNewColors[4] = { RGB(32, 32, 32), RGB(255, 255, 255), RGB(32, 32, 32), RGB(32, 32, 32) };
          // SetSysColors(4, aElements, aNewColors);
          // allowDarkModeForApp(true);
          // refreshImmersiveColorPolicyState();
          // mode = TRUE;
          // SetWindowTheme(window, L"DarkMode_Explorer", NULL);
        }

        HMENU hMenubar = CreateMenu();
        // MENUINFO Info;
        // Info.cbSize = sizeof(Info);
        // Info.fMask = MIM_BACKGROUND; // | MFT_OWNERDRAW;
        // Info.hbrBack = bgBrush; // (HBRUSH) bgBrush;
        // SetMenuInfo(hMenubar, &Info);

        /* auto brush = (HBRUSH) CreateSolidBrush(RGB(0, 0, 0));

        MENUINFO MenuInfo = {0};
        MenuInfo.cbSize = sizeof(MenuInfo);
        MenuInfo.hbrBack = brush; // Brush you want to draw
        MenuInfo.fMask = MIM_BACKGROUND;
        MenuInfo.dwStyle = MNS_AUTODISMISS; */

        SetMenu(hWnd, hMenubar);
        break;
      }

      case WM_CLOSE: {
        #if DEBUG == 1
          fclose(console);
          FreeConsole();
        #endif

        w->close();

        break;

        // if (hbrBkgnd) {
        //  DeleteObject(hbrBkgnd);
        //  hbrBkgnd = nullptr;
        // }
      }

      default:
        return DefWindowProc(hWnd, message, wParam, lParam);
        break;
    }

    return 0;
  }

} // namespace Opkit
