#ifndef WEBVIEW_H
#define WEBVIEW_H

#include "platform.h"
#include <iostream>

#ifndef WEBVIEW_API
#define WEBVIEW_API extern
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void *webview_t;

//
// Creates a new webview instance. If debug is non-zero - developer tools will
// be enabled (if the platform supports them). Window parameter can be a
// pointer to the native window handle. If it's non-null - then child WebView
// is embedded into the given parent window. Otherwise a new window is created.
// Depending on the platform, a GtkWindow, NSWindow or HWND pointer can be
// passed here.
//
WEBVIEW_API
  webview_t webview_create(int debug, void *window);

//
// Destroys a webview and closes the native window.
//
WEBVIEW_API
  void webview_destroy(webview_t w);

//
// Runs the main loop until it's terminated. After this function exits - you
// must destroy the webview.
//
WEBVIEW_API
  void webview_run(webview_t w);

//
// Stops the main loop. It is safe to call this function from another other
// background thread.
//
WEBVIEW_API
  void webview_terminate(webview_t w);

//
// Posts a function to be executed on the main thread. You normally do not need
// to call this function, unless you want to tweak the native window.
//
WEBVIEW_API
  void webview_dispatch(webview_t w, void (*fn)(webview_t w, void *arg), void *arg);

//
// Returns a native window handle pointer. When using GTK backend the pointer
// is GtkWindow pointer, when using Cocoa backend the pointer is NSWindow
// pointer, when using Win32 backend the pointer is HWND pointer.
//
WEBVIEW_API
  void *webview_get_window(webview_t w);

//
// Updates the title of the native window. Must be called from the UI thread.
//
WEBVIEW_API
  void webview_set_title(webview_t w, const char *title);

#define WEBVIEW_HINT_NONE 0  // Width and height are default size
#define WEBVIEW_HINT_MIN 1   // Width and height are minimum bounds
#define WEBVIEW_HINT_MAX 2   // Width and height are maximum bounds
#define WEBVIEW_HINT_FIXED 3 // Window size can not be changed by a user

//
// Updates native window size. See WEBVIEW_HINT constants.
//
WEBVIEW_API
  void webview_set_size(webview_t w, int width, int height,
                                  int hints);
//
// Navigates webview to the given URL. URL may be a data URI, i.e.
// "data:text/text,<html>...</html>". It is often ok not to url-encode it
// properly, webview will re-encode it for you.
//
WEBVIEW_API
  void webview_navigate(webview_t w, const char *url);

//
// Injects JavaScript code at the initialization of the new page. Every time
// the webview will open a the new page - this initialization code will be
// executed. It is guaranteed that code is executed before window.onload.
//
WEBVIEW_API
  void webview_init(webview_t w, const char *js);

//
// Evaluates arbitrary JavaScript code. Evaluation happens asynchronously, also
// the result of the expression is ignored. Use RPC bindings if you want to
// receive notifications about the results of the evaluation.
//
WEBVIEW_API
  void webview_eval(webview_t w, const char *js);

//
// Binds a native C callback so that it will appear under the given name as a
// global JavaScript function. Internally it uses webview_init(). Callback
// receives a request string and a user-provided argument pointer. Request
// string is a JSON array of all the arguments passed to the JavaScript
// function.
//
WEBVIEW_API
  void webview_ipc(
    webview_t w,
    const char *name,
    void (*fn)(const char *seq, const char *req, void *arg),
    void *arg
  );

//
// Allows to return a value from the native binding. Original request pointer
// must be provided to help internal RPC engine match requests with responses.
// If status is zero - result is expected to be a valid JSON result value.
// If status is not zero - result is an error JSON object.
//
WEBVIEW_API
  void webview_return(
    webview_t w,
    const char *seq,
    int status,
    const char *result
  );

#ifdef __cplusplus
}
#endif

#ifndef WEBVIEW_HEADER

#if !defined(WEBVIEW_GTK) && !defined(WEBVIEW_COCOA) && !defined(WEBVIEW_EDGE)
#if defined(__linux__)
#define WEBVIEW_GTK
#elif defined(__APPLE__)
#define WEBVIEW_COCOA
#elif defined(_WIN32)
#define WEBVIEW_EDGE
#else
#error "please, specify webview backend"
#endif
#endif

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <cstring>

namespace webview {
using dispatch_fn_t = std::function<void()>;
} // namespace webview

#if defined(WEBVIEW_GTK)
//
// ====================================================================
//
// This implementation uses webkit2gtk backend. It requires gtk+3.0 and
// webkit2gtk-4.0 libraries. Proper compiler flags can be retrieved via:
//
//   pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0
//
// ====================================================================
//
#include <JavaScriptCore/JavaScript.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

namespace webview {

class gtk_webkit_engine {
public:
  gtk_webkit_engine(bool debug, void *window)
      : m_window(static_cast<GtkWidget *>(window)) {
    gtk_init_check(0, NULL);
    m_window = static_cast<GtkWidget *>(window);
    if (m_window == nullptr) {
      m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    }
    g_signal_connect(G_OBJECT(m_window), "destroy",
                     G_CALLBACK(+[](GtkWidget *, gpointer arg) {
                       static_cast<gtk_webkit_engine *>(arg)->terminate();
                     }),
                     this);
    // Initialize webview widget
    m_webview = webkit_web_view_new();
    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(m_webview));
    g_signal_connect(manager, "script-message-received::external",
                     G_CALLBACK(+[](WebKitUserContentManager *,
                                    WebKitJavascriptResult *r, gpointer arg) {
                       auto *w = static_cast<gtk_webkit_engine *>(arg);
#if WEBKIT_MAJOR_VERSION >= 2 && WEBKIT_MINOR_VERSION >= 22
                       JSCValue *value =
                           webkit_javascript_result_get_js_value(r);
                       char *s = jsc_value_to_string(value);
#else
                       JSGlobalContextRef ctx =
                           webkit_javascript_result_get_global_context(r);
                       JSValueRef value = webkit_javascript_result_get_value(r);
                       JSStringRef js = JSValueToStringCopy(ctx, value, NULL);
                       size_t n = JSStringGetMaximumUTF8CStringSize(js);
                       char *s = g_new(char, n);
                       JSStringGetUTF8CString(js, s, n);
                       JSStringRelease(js);
#endif
                       w->on_message(s);
                       g_free(s);
                     }),
                     this);
    webkit_user_content_manager_register_script_message_handler(manager,
                                                                "external");
    init("window.external={invoke:s => {window.webkit.messageHandlers."
         "external.postMessage(s);}}");

    gtk_container_add(GTK_CONTAINER(m_window), GTK_WIDGET(m_webview));
    gtk_widget_grab_focus(GTK_WIDGET(m_webview));

    WebKitSettings *settings =
        webkit_web_view_get_settings(WEBKIT_WEB_VIEW(m_webview));
    webkit_settings_set_javascript_can_access_clipboard(settings, true);
    if (debug) {
      webkit_settings_set_enable_write_console_messages_to_stdout(settings,
                                                                  true);
      webkit_settings_set_enable_developer_extras(settings, true);
    }

    gtk_widget_show_all(m_window);
  }
  void *window() { return (void *)m_window; }
  void run() { gtk_main(); }
  void terminate() { gtk_main_quit(); }
  void dispatch(std::function<void()> f) {
    g_idle_add_full(G_PRIORITY_HIGH_IDLE, (GSourceFunc)([](void *f) -> int {
                      (*static_cast<dispatch_fn_t *>(f))();
                      return G_SOURCE_REMOVE;
                    }),
                    new std::function<void()>(f),
                    [](void *f) { delete static_cast<dispatch_fn_t *>(f); });
  }

  void dialog(std::string seq) {
    dispatch([=]() {
      auto result = createDialog(
        NOC_FILE_DIALOG_OPEN | NOC_FILE_DIALOG_DIR,
        NULL,
        NULL,
        NULL);

      eval("(() => {"
           "  window._ipc[" + seq + "].resolve(`" + result + "`);"
           "  delete window._ipc[" + seq + "];"
           "})();");
    });
  }

  void set_title(const std::string title) {
    gtk_window_set_title(GTK_WINDOW(m_window), title.c_str());
  }

  void set_size(int width, int height, int hints) {
    gtk_window_set_resizable(GTK_WINDOW(m_window), hints != WEBVIEW_HINT_FIXED);
    if (hints == WEBVIEW_HINT_NONE) {
      gtk_window_resize(GTK_WINDOW(m_window), width, height);
    } else if (hints == WEBVIEW_HINT_FIXED) {
      gtk_widget_set_size_request(m_window, width, height);
    } else {
      GdkGeometry g;
      g.min_width = g.max_width = width;
      g.min_height = g.max_height = height;
      GdkWindowHints h =
          (hints == WEBVIEW_HINT_MIN ? GDK_HINT_MIN_SIZE : GDK_HINT_MAX_SIZE);
      // This defines either MIN_SIZE, or MAX_SIZE, but not both:
      gtk_window_set_geometry_hints(GTK_WINDOW(m_window), nullptr, &g, h);
    }
  }

  void navigate(const std::string url) {
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(m_webview), url.c_str());
  }

  void init(const std::string js) {
    WebKitUserContentManager *manager =
        webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(m_webview));
    webkit_user_content_manager_add_script(
        manager, webkit_user_script_new(
                     js.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                     WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL));
  }

  void eval(const std::string js) {
    webkit_web_view_run_javascript(WEBKIT_WEB_VIEW(m_webview), js.c_str(), NULL,
                                   NULL, NULL);
  }

private:
  virtual void on_message(const std::string msg) = 0;
  GtkWidget *m_window;
  GtkWidget *m_webview;
};

using browser_engine = gtk_webkit_engine;

} // namespace webview

#elif defined(WEBVIEW_COCOA)

//
// ====================================================================
//
// This implementation uses Cocoa WKWebView backend on macOS. It is
// written using ObjC runtime and uses WKWebView class as a browser runtime.
// You should pass "-framework Webkit" flag to the compiler.
//
// ====================================================================
//

#include <CoreGraphics/CoreGraphics.h>
#include <objc/objc-runtime.h>

#define NSBackingStoreBuffered 2

#define NSWindowStyl0MaskHUDWindow 1 << 13
#define NSWindowStyleMaskResizable 8
#define NSWindowStyleMaskMiniaturizable 4
#define NSWindowStyleMaskTitled 1
#define NSTexturedBackgroundWindowMask 1 << 8
#define NSWindowStyleMaskTexturedBackground 1 << 8
#define NSWindowStyleMaskUnifiedTitleAndToolbar 1 << 12
#define NSWindowTitleHidden 1
#define NSFullSizeContentViewWindowMask 1 << 15
#define NSWindowStyleMaskClosable 2

#define NSApplicationActivationPolicyRegular 0

#define WKUserScriptInjectionTimeAtDocumentStart 0

namespace webview {

SEL NSSelector(const char *s) {
  return sel_registerName(s);
}

id NSClass(const char *s) {
  return (id) objc_getClass(s);
}

id NSString (const char *s) {
  return ((id(*) (id, SEL, const char *)) objc_msgSend)(
    NSClass("NSString"),
    NSSelector("stringWithUTF8String:"),
    s
  );
}

class cocoa_wkwebview_engine {
  public:

  cocoa_wkwebview_engine(bool debug, void *window) {
    // Application
    id app = ((id(*)(id, SEL))objc_msgSend)(
      NSClass("NSApplication"),
      NSSelector("sharedApplication")
    );

    ((void (*)(id, SEL, long)) objc_msgSend)(
      app,
      NSSelector("setActivationPolicy:"),
      NSApplicationActivationPolicyRegular
    );

    // Delegate
    auto cls = objc_allocateClassPair((Class) NSClass("NSResponder"), "AppDelegate", 0);

    class_addProtocol(cls, objc_getProtocol("NSTouchBarProvider"));

    class_addMethod(
      cls,
      NSSelector("applicationShouldTerminateAfterLastWindowClosed:"),
      (IMP)(+[](id, SEL, id) -> BOOL { return 1; }),
      "c@:@"
    );

    class_addMethod(
      cls,
      NSSelector("menuItemSelected:"),
      (IMP)(+[](id self, SEL _cmd, id item) {
        auto w = (cocoa_wkwebview_engine*) objc_getAssociatedObject(self, "webview");
        assert(w);

        auto vec = getMenuItemDetails(item);
        auto title = vec[0];
        auto state = vec[1];
        auto parent = vec[2];
        auto seq = vec[3];

        w->eval("(() => {"
          "  const detail = {"
          "    title: '" + title + "',"
          "    parent: '" + parent + "',"
          "    state: '" + state + "'"
          "  };"

          "  if (" + seq + " > 0) {"
          "    window._ipc[" + seq + "].resolve(detail);"
          "    delete window._ipc[" + seq + "];"
          "    return;"
          "  }"

          "  const event = new window.CustomEvent('menuItemSelected', { detail });"
          "  window.dispatchEvent(event);"
          "})()"
        );
      }),
      "v@:@:@:"
    );

    class_addMethod(
      cls,
      NSSelector("themeChangedOnMainThread"),
      (IMP)(+[](id self) {
        auto w = (cocoa_wkwebview_engine*) objc_getAssociatedObject(self, "webview");
        assert(w);

        w->eval("(() => {"
          "  const event = new window.CustomEvent('themeChanged');"
          "  window.dispatchEvent(event);"
          "})()"
        );
      }),
      "v"
    );

    class_addMethod(
      cls,
      NSSelector("userContentController:didReceiveScriptMessage:"),
      (IMP)(+[](id self, SEL, id, id msg) {
        auto w = (cocoa_wkwebview_engine*) objc_getAssociatedObject(self, "webview");
        assert(w);

        w->on_message(((const char* (*)(id, SEL))objc_msgSend)(
          ((id (*)(id, SEL)) objc_msgSend)(
            msg,
            NSSelector("body")
          ),
          NSSelector("UTF8String")
        ));
      }),
      "v@:@@"
    );

    objc_registerClassPair(cls);

    auto delegate = ((id(*)(id, SEL))objc_msgSend)(
      (id)cls,
      NSSelector("new")
    );

    objc_setAssociatedObject(
      delegate,
      "webview",
      (id)this,
      OBJC_ASSOCIATION_ASSIGN
    );

    ((void (*)(id, SEL, id))objc_msgSend)(
      app,
      sel_registerName("setDelegate:"),
      delegate
    );

    addListenerThemeChange(delegate);

    // Main window
    if (window == nullptr) {
      m_window = ((id(*)(id, SEL))objc_msgSend)(
        NSClass("NSWindow"),
        NSSelector("alloc")
      );
      
      m_window = ((id(*)(id, SEL, CGRect, int, unsigned long, int))objc_msgSend)(
        m_window,
        NSSelector("initWithContentRect:styleMask:backing:defer:"),
        CGRectMake(0, 0, 0, 0),
        0,
        NSBackingStoreBuffered,
        0
      );
    } else {
      m_window = (id)window;
    }

    // Webview
    auto config = ((id (*)(id, SEL)) objc_msgSend)(
      NSClass("WKWebViewConfiguration"),
      NSSelector("new")
    );

    m_manager = ((id (*)(id, SEL)) objc_msgSend)(
      config,
      NSSelector("userContentController")
    );

    m_webview = ((id (*)(id, SEL)) objc_msgSend)(
      NSClass("WKWebView"),
      NSSelector("alloc")
    );

    if (debug) {
      //
      // [[config preferences] setValue:@YES forKey:@"developerExtrasEnabled"];
      // 
      ((id(*)(id, SEL, id, id))objc_msgSend)(
        ((id(*)(id, SEL))objc_msgSend)(
          config,
          NSSelector("preferences")
        ),
        NSSelector("setValue:forKey:"),
        ((id(*)(id, SEL, BOOL))objc_msgSend)(
          NSClass("NSNumber"),
          NSSelector("numberWithBool:"),
          1),
        NSString("developerExtrasEnabled")
      );
    }

    //
    // [[config preferences] setValue:@YES forKey:@"fullScreenEnabled"];
    //
    ((id(*)(id, SEL, id, id))objc_msgSend)(
      ((id(*)(id, SEL))objc_msgSend)(
        config,
        NSSelector("preferences")
      ),
      NSSelector("setValue:forKey:"),
      ((id(*)(id, SEL, BOOL))objc_msgSend)(
        NSClass("NSNumber"),
        NSSelector("numberWithBool:"),
        1
      ),
      NSString("fullScreenEnabled")
    );

    //
    // [[config preferences] setValue:@YES forKey:@"allowFileAccessFromFileURLs"];
    //
    ((id(*)(id, SEL, id, id))objc_msgSend)(
      ((id(*)(id, SEL))objc_msgSend)(
        config,
        NSSelector("preferences")
      ),
      NSSelector("setValue:forKey:"),
      ((id(*)(id, SEL, BOOL))objc_msgSend)(
        NSClass("NSNumber"),
        NSSelector("numberWithBool:"),
        1
      ),
      NSString("allowFileAccessFromFileURLs")
    );

    //
    // [[config preferences] setValue:@YES forKey:@"javaScriptCanAccessClipboard"];
    //
    ((id(*)(id, SEL, id, id))objc_msgSend)(
      ((id(*)(id, SEL))objc_msgSend)(
        config,
        NSSelector("preferences")
      ),
      NSSelector("setValue:forKey:"),
      ((id(*)(id, SEL, BOOL))objc_msgSend)(
        NSClass("NSNumber"),
        NSSelector("numberWithBool:"),
        1
      ),
      NSString("javaScriptCanAccessClipboard")
    );

    //
    // [[config preferences] setValue:@YES forKey:@"DOMPasteAllowed"];
    //
    ((id(*)(id, SEL, id, id))objc_msgSend)(
      ((id(*)(id, SEL))objc_msgSend)(
        config,
        NSSelector("preferences")
      ),
      NSSelector("setValue:forKey:"),
      ((id(*)(id, SEL, BOOL))objc_msgSend)(
        NSClass("NSNumber"),
        NSSelector("numberWithBool:"),
        1
      ),
      NSString("DOMPasteAllowed")
    );

    ((void (*)(id, SEL, CGRect, id))objc_msgSend)(
      m_webview,
      NSSelector("initWithFrame:configuration:"),
      CGRectMake(0, 0, 0, 0),
      config
    );

    // initTitleBar();

    ((void (*)(id, SEL, id, id))objc_msgSend)(
      m_manager,
      NSSelector("addScriptMessageHandler:name:"),
      delegate,
      NSString("external")
    );

    init(R"script(
      window.external = {
        invoke: s => {
          window.webkit.messageHandlers.external.postMessage(s)
        }
      }
     )script");

    ((void (*)(id, SEL, id))objc_msgSend)(
      m_window,
      NSSelector("setContentView:"),
      m_webview
    );

    ((void (*)(id, SEL, id))objc_msgSend)(
      m_window,
      NSSelector("makeKeyAndOrderFront:"),
      nullptr
    );
  }

  ~cocoa_wkwebview_engine() { close(); }
  void *window() { return (void *)m_window; }

  void terminate() {
    close();

    ((void (*)(id, SEL, id))objc_msgSend)(
      NSClass("NSApp"),
      NSSelector("terminate:"),
      nullptr
    );
  }

  void run() {
    id app = ((id(*)(id, SEL))objc_msgSend)(
      NSClass("NSApplication"),
      NSSelector("sharedApplication")
    );

    dispatch([&]() {
      ((void (*)(id, SEL, BOOL))objc_msgSend)(
        app,
        NSSelector("activateIgnoringOtherApps:"),
        1
      );
    });

    ((void (*)(id, SEL))objc_msgSend)(app, NSSelector("run"));
  }

  void dispatch(std::function<void()> f) {
    dispatch_async_f(dispatch_get_main_queue(), new dispatch_fn_t(f),
     (dispatch_function_t)([](void *arg) {
       auto f = static_cast<dispatch_fn_t *>(arg);
       (*f)();
       delete f;
     })
    );
  }

  void set_title(const std::string title) {
    ((void (*)(id, SEL, id))objc_msgSend)(
      m_window,
      NSSelector("setTitle:"),
      ((id(*)(id, SEL, const char *))objc_msgSend)(
        NSClass("NSString"),
        NSSelector("stringWithUTF8String:"),
        title.c_str()
      )
    );

    setTitle(m_window);
  }

  void set_size(int width, int height, int hints) {
    auto style = 
      NSWindowStyleMaskClosable |
      NSWindowStyleMaskTitled |
      // NSFullSizeContentViewWindowMask |
      // NSTexturedBackgroundWindowMask |
      // NSWindowStyleMaskTexturedBackground |
      // NSWindowTitleHidden |
      NSWindowStyleMaskMiniaturizable;

    if (hints != WEBVIEW_HINT_FIXED) {
      style = style | NSWindowStyleMaskResizable;
    }

    ((void (*)(id, SEL, unsigned long)) objc_msgSend)(
      m_window,
      NSSelector("setStyleMask:"),
      style
    );

    if (hints == WEBVIEW_HINT_MIN) {
      ((void (*)(id, SEL, CGSize)) objc_msgSend)(
        m_window,
        NSSelector("setContentMinSize:"),
        CGSizeMake(width, height)
      );
    } else if (hints == WEBVIEW_HINT_MAX) {
      ((void (*)(id, SEL, CGSize)) objc_msgSend)(
        m_window,
        NSSelector("setContentMaxSize:"),
        CGSizeMake(width, height)
      );
    } else {
      ((void (*)(id, SEL, CGRect, BOOL, BOOL)) objc_msgSend)(
        m_window,
        NSSelector("setFrame:display:animate:"),
        CGRectMake(0, 0, width, height),
        1,
        0
      );
    }

    ((void (*)(id, SEL))objc_msgSend)(m_window, NSSelector("center"));
    ((void (*)(id, SEL))objc_msgSend)(m_window, NSSelector("setHasShadow:"));

    // setWindowColor(m_window, 0.1, 0.1, 0.1, 0.0);

    ((void (*)(id, SEL, BOOL))objc_msgSend)(m_window, NSSelector("setTitlebarAppearsTransparent:"), 1);
    ((void (*)(id, SEL, BOOL))objc_msgSend)(m_window, NSSelector("setOpaque:"), 1);

    // ((void (*)(id, SEL, BOOL))objc_msgSend)(m_window, "setTitleVisibility:"_sel, 1);
    // ((void (*)(id, SEL, BOOL))objc_msgSend)(m_window, "setMovableByWindowBackground:"_sel, 1);
    // setWindowButtonsOffset:NSMakePoint(12, 10)
    // setTitleVisibility:NSWindowTitleHidden
  }

  void navigate(const std::string url) {
    auto nsurl = ((id(*)(id, SEL, id))objc_msgSend)(
      NSClass("NSURL"),
      NSSelector("URLWithString:"),
      ((id(*)(id, SEL, const char *))objc_msgSend)(
        NSClass("NSString"),
        NSSelector("stringWithUTF8String:"),
        url.c_str()
      )
    );

    ((void (*)(id, SEL, id))objc_msgSend)(
      m_webview,
      NSSelector("loadRequest:"),
      ((id(*)(id, SEL, id))objc_msgSend)(
        NSClass("NSURLRequest"),
        NSSelector("requestWithURL:"),
        nsurl
      )
    );
  }

  void init(const std::string js) {
    // Equivalent Obj-C:
    // [m_manager addUserScript:[[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:js.c_str()] injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]]
    ((void (*)(id, SEL, id)) objc_msgSend)(
      m_manager,
      NSSelector("addUserScript:"),
      ((id (*)(id, SEL, id, long, BOOL)) objc_msgSend)(
        ((id (*)(id, SEL)) objc_msgSend)(
          NSClass("WKUserScript"),
          NSSelector("alloc")
        ),
        NSSelector("initWithSource:injectionTime:forMainFrameOnly:"),
        ((id (*)(id, SEL, const char *)) objc_msgSend)(
          NSClass("NSString"),
          NSSelector("stringWithUTF8String:"),
          js.c_str()
        ),
        WKUserScriptInjectionTimeAtDocumentStart,
        1
      )
    );
  }

  void dialog(std::string seq) {
    dispatch([=]() {
      auto result = createDialog(
        NOC_FILE_DIALOG_OPEN | NOC_FILE_DIALOG_DIR,
        NULL,
        NULL,
        NULL);

      eval("(() => {"
           "  window._ipc[" + seq + "].resolve(`" + result + "`);"
           "  delete window._ipc[" + seq + "];"
           "})();");
    });
  }

  void eval(const std::string js) {
    ((void (*)(id, SEL, id, id)) objc_msgSend)(
      m_webview,
      NSSelector("evaluateJavaScript:completionHandler:"),
      ((id(*)(id, SEL, const char *)) objc_msgSend)(
        NSClass("NSString"),
        NSSelector("stringWithUTF8String:"),
        js.c_str()
      ),
      nullptr
    );
  }

private:
  virtual void on_message(const std::string msg) = 0;
  void close() { ((void (*)(id, SEL))objc_msgSend)(m_window, NSSelector("close")); }
  id m_window;
  id m_webview;
  id m_manager;
};

using browser_engine = cocoa_wkwebview_engine;

} // namespace webview

#elif defined(WEBVIEW_EDGE)

//
// ====================================================================
//
// This implementation uses Win32 API to create a native window. It can
// use either EdgeHTML or Edge/Chromium backend as a browser engine.
//
// ====================================================================
//

#define WIN32_LEAN_AND_MEAN
#include <Shlwapi.h>
#include <codecvt>
#include <stdlib.h>
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Shlwapi.lib")

// EdgeHTML headers and libs
#include <objbase.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.UI.Interop.h>
#pragma comment(lib, "windowsapp")

// Edge/Chromium headers and libs
#include "webview2.h"
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace webview {

using msg_cb_t = std::function<void(const std::string)>;

// Common interface for EdgeHTML and Edge/Chromium
class browser {
public:
  virtual ~browser() = default;
  virtual bool embed(HWND, bool, msg_cb_t) = 0;
  virtual void navigate(const std::string url) = 0;
  virtual void eval(const std::string js) = 0;
  virtual void init(const std::string js) = 0;
  virtual void resize(HWND) = 0;
};

//
// EdgeHTML browser engine
//
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Web::UI;
using namespace Windows::Web::UI::Interop;

class edge_html : public browser {
public:
  bool embed(HWND wnd, bool debug, msg_cb_t cb) override {
    init_apartment(winrt::apartment_type::single_threaded);
    auto process = WebViewControlProcess();
    auto op = process.CreateWebViewControlAsync(reinterpret_cast<int64_t>(wnd),
                                                Rect());
    if (op.Status() != AsyncStatus::Completed) {
      handle h(CreateEvent(nullptr, false, false, nullptr));
      op.Completed([h = h.get()](auto, auto) { SetEvent(h); });
      HANDLE hs[] = {h.get()};
      DWORD i;
      CoWaitForMultipleHandles(COWAIT_DISPATCH_WINDOW_MESSAGES |
                                   COWAIT_DISPATCH_CALLS |
                                   COWAIT_INPUTAVAILABLE,
                               INFINITE, 1, hs, &i);
    }
    m_webview = op.GetResults();
    m_webview.Settings().IsScriptNotifyAllowed(true);
    m_webview.IsVisible(true);
    m_webview.ScriptNotify([=](auto const &sender, auto const &args) {
      std::string s = winrt::to_string(args.Value());
      cb(s.c_str());
    });
    m_webview.NavigationStarting([=](auto const &sender, auto const &args) {
      m_webview.AddInitializeScript(winrt::to_hstring(init_js));
    });
    init("window.external.invoke = s => window.external.notify(s)");
    return true;
  }

  void navigate(const std::string url) override {
    Uri uri(winrt::to_hstring(url));
    m_webview.Navigate(uri);
  }

  void init(const std::string js) override {
    init_js = init_js + "(function(){" + js + "})();";
  }

  void eval(const std::string js) override {
    m_webview.InvokeScriptAsync(
        L"eval", single_threaded_vector<hstring>({winrt::to_hstring(js)}));
  }

  void resize(HWND wnd) override {
    if (m_webview == nullptr) {
      return;
    }
    RECT r;
    GetClientRect(wnd, &r);
    Rect bounds(r.left, r.top, r.right - r.left, r.bottom - r.top);
    m_webview.Bounds(bounds);
  }

private:
  WebViewControl m_webview = nullptr;
  std::string init_js = "";
};

//
// Edge/Chromium browser engine
//
class edge_chromium : public browser {
public:
  bool embed(HWND wnd, bool debug, msg_cb_t cb) override {
    CoInitializeEx(nullptr, 0);
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    flag.test_and_set();

    char currentExePath[MAX_PATH];
    GetModuleFileNameA(NULL, currentExePath, MAX_PATH);
    char *currentExeName = PathFindFileNameA(currentExePath);

    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wideCharConverter;
    std::wstring userDataFolder =
        wideCharConverter.from_bytes(std::getenv("APPDATA"));
    std::wstring currentExeNameW = wideCharConverter.from_bytes(currentExeName);

    HRESULT res = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, (userDataFolder + L"/" + currentExeNameW).c_str(), nullptr,
        new webview2_com_handler(wnd, cb,
                                 [&](ICoreWebView2Controller *controller) {
                                   m_controller = controller;
                                   m_controller->get_CoreWebView2(&m_webview);
                                   m_webview->AddRef();
                                   flag.clear();
                                 }));
    if (res != S_OK) {
      CoUninitialize();
      return false;
    }
    MSG msg = {};
    while (flag.test_and_set() && GetMessage(&msg, NULL, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    init("window.external = { invoke: s => window.chrome.webview.postMessage(s) }");
    return true;
  }

  void resize(HWND wnd) override {
    if (m_controller == nullptr) {
      return;
    }
    RECT bounds;
    GetClientRect(wnd, &bounds);
    m_controller->put_Bounds(bounds);
  }

  void navigate(const std::string url) override {
    auto wurl = to_lpwstr(url);
    m_webview->Navigate(wurl);
    delete[] wurl;
  }

  void init(const std::string js) override {
    LPCWSTR wjs = to_lpwstr(js);
    m_webview->AddScriptToExecuteOnDocumentCreated(wjs, nullptr);
    delete[] wjs;
  }

  void eval(const std::string js) override {
    LPCWSTR wjs = to_lpwstr(js);
    m_webview->ExecuteScript(wjs, nullptr);
    delete[] wjs;
  }

private:
  LPWSTR to_lpwstr(const std::string s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    wchar_t *ws = new wchar_t[n];
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws, n);
    return ws;
  }

  ICoreWebView2 *m_webview = nullptr;
  ICoreWebView2Controller *m_controller = nullptr;

  class webview2_com_handler
      : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        public ICoreWebView2WebMessageReceivedEventHandler,
        public ICoreWebView2PermissionRequestedEventHandler {
    using webview2_com_handler_cb_t =
        std::function<void(ICoreWebView2Controller *)>;

  public:
    webview2_com_handler(HWND hwnd, msg_cb_t msgCb,
                         webview2_com_handler_cb_t cb)
        : m_window(hwnd), m_msgCb(msgCb), m_cb(cb) {}
    ULONG STDMETHODCALLTYPE AddRef() { return 1; }
    ULONG STDMETHODCALLTYPE Release() { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID *ppv) {
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res,
                                     ICoreWebView2Environment *env) {
      env->CreateCoreWebView2Controller(m_window, this);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT res,
                                     ICoreWebView2Controller *controller) {
      controller->AddRef();

      ICoreWebView2 *webview;
      ::EventRegistrationToken token;
      controller->get_CoreWebView2(&webview);
      webview->add_WebMessageReceived(this, &token);
      webview->add_PermissionRequested(this, &token);

      m_cb(controller);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) {
      LPWSTR message;
      args->TryGetWebMessageAsString(&message);

      std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> wideCharConverter;
      m_msgCb(wideCharConverter.to_bytes(message));
      sender->PostWebMessageAsString(message);

      CoTaskMemFree(message);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2 *sender,
           ICoreWebView2PermissionRequestedEventArgs *args) {
      COREWEBVIEW2_PERMISSION_KIND kind;
      args->get_PermissionKind(&kind);
      if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
        args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
      }
      return S_OK;
    }

  private:
    HWND m_window;
    msg_cb_t m_msgCb;
    webview2_com_handler_cb_t m_cb;
  };
};

class win32_edge_engine {
public:
  win32_edge_engine(bool debug, void *window) {
    if (window == nullptr) {
      HINSTANCE hInstance = GetModuleHandle(nullptr);
      HICON icon = (HICON)LoadImage(
          hInstance, IDI_APPLICATION, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

      WNDCLASSEX wc;
      ZeroMemory(&wc, sizeof(WNDCLASSEX));
      wc.cbSize = sizeof(WNDCLASSEX);
      wc.hInstance = hInstance;
      wc.lpszClassName = "webview";
      wc.hIcon = icon;
      wc.hIconSm = icon;
      wc.lpfnWndProc =
          (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> int {
            auto w = (win32_edge_engine *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            switch (msg) {
            case WM_SIZE:
              w->m_browser->resize(hwnd);
              break;
            case WM_CLOSE:
              DestroyWindow(hwnd);
              break;
            case WM_DESTROY:
              w->terminate();
              break;
            case WM_GETMINMAXINFO: {
              auto lpmmi = (LPMINMAXINFO)lp;
              if (w == nullptr) {
                return 0;
              }
              if (w->m_maxsz.x > 0 && w->m_maxsz.y > 0) {
                lpmmi->ptMaxSize = w->m_maxsz;
                lpmmi->ptMaxTrackSize = w->m_maxsz;
              }
              if (w->m_minsz.x > 0 && w->m_minsz.y > 0) {
                lpmmi->ptMinTrackSize = w->m_minsz;
              }
            } break;
            default:
              return DefWindowProc(hwnd, msg, wp, lp);
            }
            return 0;
          });
      RegisterClassEx(&wc);
      m_window = CreateWindow("webview", "", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 640, 480, nullptr, nullptr,
                              GetModuleHandle(nullptr), nullptr);
      SetWindowLongPtr(m_window, GWLP_USERDATA, (LONG_PTR)this);
    } else {
      m_window = *(static_cast<HWND *>(window));
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    ShowWindow(m_window, SW_SHOW);
    UpdateWindow(m_window);
    SetFocus(m_window);

    auto cb =
        std::bind(&win32_edge_engine::on_message, this, std::placeholders::_1);

    if (!m_browser->embed(m_window, debug, cb)) {
      m_browser = std::make_unique<webview::edge_html>();
      m_browser->embed(m_window, debug, cb);
    }

    m_browser->resize(m_window);
  }

  void run() {
    MSG msg;
    BOOL res;
    while ((res = GetMessage(&msg, nullptr, 0, 0)) != -1) {
      if (msg.hwnd) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        continue;
      }
      if (msg.message == WM_APP) {
        auto f = (dispatch_fn_t *)(msg.lParam);
        (*f)();
        delete f;
      } else if (msg.message == WM_QUIT) {
        return;
      }
    }
  }
  void *window() { return (void *)m_window; }
  void terminate() { PostQuitMessage(0); }
  void dispatch(dispatch_fn_t f) {
    PostThreadMessage(m_main_thread, WM_APP, 0, (LPARAM) new dispatch_fn_t(f));
  }

  void set_title(const std::string title) {
    SetWindowText(m_window, title.c_str());
  }

  void set_size(int width, int height, int hints) {
    auto style = GetWindowLong(m_window, GWL_STYLE);
    if (hints == WEBVIEW_HINT_FIXED) {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    } else {
      style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLong(m_window, GWL_STYLE, style);

    if (hints == WEBVIEW_HINT_MAX) {
      m_maxsz.x = width;
      m_maxsz.y = height;
    } else if (hints == WEBVIEW_HINT_MIN) {
      m_minsz.x = width;
      m_minsz.y = height;
    } else {
      RECT r;
      r.left = r.top = 0;
      r.right = width;
      r.bottom = height;
      AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, 0);
      SetWindowPos(
          m_window, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED);
      m_browser->resize(m_window);
    }
  }

  void navigate(const std::string url) { m_browser->navigate(url); }
  void eval(const std::string js) { m_browser->eval(js); }
  void init(const std::string js) { m_browser->init(js); }

private:
  virtual void on_message(const std::string msg) = 0;

  HWND m_window;
  POINT m_minsz = POINT{0, 0};
  POINT m_maxsz = POINT{0, 0};
  DWORD m_main_thread = GetCurrentThreadId();
  std::unique_ptr<webview::browser> m_browser =
      std::make_unique<webview::edge_chromium>();
};

using browser_engine = win32_edge_engine;
} // namespace webview

#endif /* WEBVIEW_GTK, WEBVIEW_COCOA, WEBVIEW_EDGE */

namespace webview {

class webview : public browser_engine {
public:
  webview(bool debug = false, void *wnd = nullptr)
      : browser_engine(debug, wnd) {}

  void navigate(const std::string url) {
    browser_engine::navigate(url);
  }

  using binding_t = std::function<void(std::string, std::string, void *)>;
  using binding_ctx_t = std::pair<binding_t *, void *>;

  using sync_binding_t = std::function<void(std::string, std::string)>;
  using sync_binding_ctx_t = std::pair<webview *, sync_binding_t>;

  void ipc(const std::string name, sync_binding_t fn) {
    ipc(
      name,
      [](std::string seq, std::string req, void *arg) {
        auto pair = static_cast<sync_binding_ctx_t *>(arg);
        pair->second(seq, req);
      },
      new sync_binding_ctx_t(this, fn)
    );
  }

  void ipc(const std::string name, binding_t f, void *arg) {
    auto js = "(function() { const name = '" + name + "';" + R"(
      const IPC = window._ipc = (window._ipc || { nextSeq: 1 });

      window[name] = (value) => {
        const seq = IPC.nextSeq++
        const promise = new Promise((resolve, reject) => {
          IPC[seq] = {
            resolve: resolve,
            reject: reject,
          }
        })

        let encoded

        if (name === 'contextMenu') {
          encoded = Object
            .entries(value)
            .flatMap(o => o.join(':'))
            .join('_')
        } else {
          try {
            encoded = btoa(JSON.stringify(value))
          } catch (err) {
            return Promise.reject(err.message)
          }
        }

        window.external.invoke(`ipc;${seq};${name};${encoded}`)
        return promise
      }
    })())";

    init(js);
    bindings[name] = new binding_ctx_t(new binding_t(f), arg);
  }

  void resolve(const std::string msg) {
    dispatch([=]() {
      eval("(() => {"
           "  const data = `" + msg + "`.trim().split(';');"
           "  const internal = data[0] === 'internal';"
           "  const status = Number(data[1]);"
           "  const seq = Number(data[2]);"
           "  const method = status === 0 ? 'resolve' : 'reject';"
           "  const value = internal ? data[3] : JSON.parse(atob(data[3]));"
           "  window._ipc[seq][method](value);"
           "  window._ipc[seq] = undefined;"
           "})()");
    });
  }

  void emit(const std::string event, const std::string data) {
    dispatch([=]() {
      eval("(() => {"
          "  let detail;"
          "  try {"
          "    detail = JSON.parse(atob(`" + data + "`));"
          "  } catch (err) {"
          "    console.error(`Unable to parse (${detail})`);"
          "    return;"
          "  }"
          "  const event = new window.CustomEvent('" + event + "', { detail });"
          "  window.dispatchEvent(event);"
          "})()");
    });
  }

private:
  void on_message(const std::string msg) {
    auto parts = split(msg, ';');

    auto seq = parts[1];
    auto name = parts[2];
    auto args = parts[3];

    if (bindings.find(name) == bindings.end()) {
      return;
    }

    auto fn = bindings[name];
    (*fn->first)(seq, args, fn->second);
  }
  std::map<std::string, binding_ctx_t *> bindings;
};
} // namespace webview

WEBVIEW_API webview_t webview_create(int debug, void *wnd) {
  return new webview::webview(debug, wnd);
}

WEBVIEW_API void webview_destroy(webview_t w) {
  delete static_cast<webview::webview *>(w);
}

WEBVIEW_API void webview_run(webview_t w) {
  static_cast<webview::webview *>(w)->run();
}

WEBVIEW_API void webview_terminate(webview_t w) {
  static_cast<webview::webview *>(w)->terminate();
}

WEBVIEW_API void webview_dispatch(webview_t w, void (*fn)(webview_t, void *),
                                  void *arg) {
  static_cast<webview::webview *>(w)->dispatch([=]() { fn(w, arg); });
}

WEBVIEW_API void *webview_get_window(webview_t w) {
  return static_cast<webview::webview *>(w)->window();
}

WEBVIEW_API void webview_set_title(webview_t w, const char *title) {
  static_cast<webview::webview *>(w)->set_title(title);
}

WEBVIEW_API void webview_set_size(webview_t w, int width, int height,
                                  int hints) {
  static_cast<webview::webview *>(w)->set_size(width, height, hints);
}

WEBVIEW_API void webview_navigate(webview_t w, const char *url) {
  static_cast<webview::webview *>(w)->navigate(url);
}

WEBVIEW_API void webview_init(webview_t w, const char *js) {
  static_cast<webview::webview *>(w)->init(js);
}

WEBVIEW_API void webview_eval(webview_t w, const char *js) {
  static_cast<webview::webview *>(w)->eval(js);
}

WEBVIEW_API void webview_ipc(
  webview_t w,
  const char *name,
  void (*fn)(const char *seq, const char *req, void *arg),
  void *arg) {
  static_cast<webview::webview *>(w)->ipc(
    name,
    [=](std::string seq, std::string req, void *arg) {
      fn(seq.c_str(), req.c_str(), arg);
    },
    arg
  );
}

WEBVIEW_API void webview_return(
  webview_t w,
  const char *seq,
  int status,
  const char *result
  ) {
  static_cast<webview::webview *>(w)->resolve(result);
}

#endif /* WEBVIEW_HEADER */

#endif /* WEBVIEW_H */
