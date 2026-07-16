// Precompiled header for the WinUI 3 (C++/WinRT) tray app.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>

// <winuser.h> (pulled in by windows.h) defines GetCurrentTime() as a macro
// (-> GetTickCount()). XAML's Microsoft.UI.Xaml.Media.Animation Timeline has a real
// GetCurrentTime() method, so the macro mangles the generated C++/WinRT projection --
// it eats the produce<> wrapper's `result` parameter, giving C4002 then a cascade of
// C3861/C2065 "'result' not found". Undo it before the winrt/XAML headers below; the
// app never calls the Win32 GetCurrentTime macro (use GetTickCount64 for ticks).
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
// urnetwork:// protocol activation: AppInstance (single-instancing + activation
// redirection) and the activation args it hands back.
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Interop.h>

// IWindowNative: the HWND behind a WinUI 3 Window (AppController::ShowWindow does
// window_.try_as<IWindowNative>()). This is the interop ABI header, not a winrt/
// projection, so include it after the winrt/Microsoft.UI.Xaml headers above.
#include <microsoft.ui.xaml.window.h>

// IInitializeWithWindow: associates a picker/popup (FileOpenPicker, etc.) with the
// app's owner HWND on desktop — MainWindow::OnAddExcludedApp does
// picker.as<::IInitializeWithWindow>()->Initialize(hwnd). Declared in the Shell header,
// a Win32 interop interface (not a winrt/ projection), like IWindowNative above.
#include <shobjidl_core.h>
