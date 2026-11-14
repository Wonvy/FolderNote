#include "desktop_groups_winui.h"
#include "desktop_box_icons.h"
#include "desktop_groups_render.h"
#include "core.h"
#include "language.h"
#include <MddBootstrap.h>
#include <commctrl.h>
#include <wincodec.h>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>
#include <windowsx.h>

namespace desktop_groups::winui {
namespace {

namespace ux = ::winrt::Microsoft::UI::Xaml;
namespace controls = ux::Controls;
namespace imaging = ::winrt::Windows::Graphics::Imaging;
namespace dispatch = ::winrt::Microsoft::UI::Dispatching;
namespace composition = ::winrt::Microsoft::UI::Composition;
namespace backdrops = composition::SystemBackdrops;
using ::winrt::Windows::Foundation::IInspectable;
using ::winrt::Windows::Foundation::Point;
using ::winrt::Windows::Foundation::Rect;

void logFailure(const wchar_t *operation, const ::winrt::hresult_error &error) {
  const std::wstring message = std::wstring(L"FolderNote WinUI: ") + operation + L": " +
      error.message().c_str() + L"\n";
  OutputDebugStringW(message.c_str());
}

struct IslandApplication : ux::ApplicationT<IslandApplication, ux::Markup::IXamlMetadataProvider> {
  ux::Markup::IXamlMetadataProvider provider =
      ux::XamlTypeInfo::XamlControlsXamlMetaDataProvider{}.as<ux::Markup::IXamlMetadataProvider>();

  ux::Markup::IXamlType GetXamlType(const ::winrt::Windows::UI::Xaml::Interop::TypeName &type) {
    return provider.GetXamlType(type);
  }

  ux::Markup::IXamlType GetXamlType(const ::winrt::hstring &name) {
    return provider.GetXamlType(name);
  }

  ::winrt::com_array<ux::Markup::XmlnsDefinition> GetXmlnsDefinitions() {
    return provider.GetXmlnsDefinitions();
  }
};

auto windowsColor(COLORREF color, BYTE alpha = 255) {
  return ::winrt::Windows::UI::Color{alpha, GetRValue(color), GetGValue(color), GetBValue(color)};
}

auto solidBrush(COLORREF color, BYTE alpha = 255) {
  return ux::Media::SolidColorBrush{windowsColor(color, alpha)};
}

struct BoxAcrylic : ux::Media::SystemBackdropT<BoxAcrylic> {
  using SystemBackdropT::OnTargetConnected;
  using SystemBackdropT::OnTargetDisconnected;
  backdrops::DesktopAcrylicController controller{nullptr};
  backdrops::SystemBackdropConfiguration configuration{nullptr};
  COLORREF tint{};
  bool dark = false;

  void appearance(bool darkTheme, COLORREF color) {
    if (dark == darkTheme && tint == color) return;
    dark = darkTheme;
    tint = color;
    applyColors();
  }

  void applyColors() {
    if (!controller) return;
    configuration.Theme(dark ? backdrops::SystemBackdropTheme::Dark : backdrops::SystemBackdropTheme::Light);
    controller.TintColor(windowsColor(tint));
    controller.FallbackColor(windowsColor(tint));
  }

  void OnTargetConnected(const composition::ICompositionSupportsSystemBackdrop &target, const ux::XamlRoot &root) {
    SystemBackdropT::OnTargetConnected(target, root);
    configuration = backdrops::SystemBackdropConfiguration{};
    configuration.IsInputActive(true);
    controller = backdrops::DesktopAcrylicController{};
    controller.Kind(backdrops::DesktopAcrylicKind::Thin);
    controller.TintOpacity(0.15f);
    controller.LuminosityOpacity(0.35f);
    controller.SetSystemBackdropConfiguration(configuration);
    applyColors();
    if (!controller.AddSystemBackdropTarget(target)) {
      controller.Close();
      controller = nullptr;
      configuration = nullptr;
    }
  }

  void OnTargetDisconnected(const composition::ICompositionSupportsSystemBackdrop &target) {
    if (controller) {
      controller.RemoveSystemBackdropTarget(target);
      controller.Close();
      controller = nullptr;
      configuration = nullptr;
    }
    SystemBackdropT::OnTargetDisconnected(target);
  }
};

template <typename Result>
Result visualChild(const ux::DependencyObject &root) {
  if (auto node = root.try_as<Result>()) return node;
  for (int i = 0, count = ux::Media::VisualTreeHelper::GetChildrenCount(root); i < count; ++i) {
    if (auto node = visualChild<Result>(ux::Media::VisualTreeHelper::GetChild(root, i))) return node;
  }
  return nullptr;
}

constexpr double contentLeftPadding = 10, contentRightPadding = 12, contentTopPadding = 10;
constexpr double viewLeftPadding = 12, pageFooterHeight = 24;

struct PageMetrics {
  int width = 1, height = 1, iconSize = 1, columns = 1, rows = 1;
};

PageMetrics measurePage(HWND parent, bool listMode, int iconSize, int headerHeight, int cornerRadius, int spacing) {
  RECT viewport{};
  const auto dpi = GetDpiForWindow(parent);
  if (!dpi || !GetClientRect(parent, &viewport)) return {};
  const double dipPerPixel = 96.0 / dpi;
  const auto width = viewport.right * dipPerPixel - contentLeftPadding - contentRightPadding - viewLeftPadding;
  const auto height = viewport.bottom * dipPerPixel - headerHeight - contentTopPadding -
      std::max(10, cornerRadius) - pageFooterHeight;
  PageMetrics metrics;
  metrics.width = std::max(1, std::min(static_cast<int>(width), std::max(84, iconSize + 34) + spacing));
  metrics.height = std::max(1, std::min(static_cast<int>(height), (listMode ? 40 : iconSize + 54) + spacing));
  metrics.iconSize = std::max(1, std::min({listMode ? 18 : iconSize, metrics.width - 12,
                                          metrics.height - (listMode ? 8 : 49)}));
  metrics.columns = listMode ? 1 : std::max(1, static_cast<int>(width / metrics.width));
  metrics.rows = std::max(1, static_cast<int>(height / metrics.height));
  return metrics;
}

ux::DataTemplate itemTemplate(bool listMode, int iconSize, int pageHeight = 0) {
  const auto size = std::to_wstring(pageHeight || !listMode ? iconSize : 18);
  const auto image = L"<Image x:Name='FileImage' Width='" + size + L"' Height='" + size +
      L"' Stretch='Uniform' VerticalAlignment='Center'/>";
  const auto label = listMode ?
      L"<TextBlock x:Name='FileName' Grid.Column='1' Margin='10,0,0,0' VerticalAlignment='Center' TextTrimming='CharacterEllipsis'/>" :
      L"<TextBlock x:Name='FileName' Margin='0,5,0,0' TextAlignment='Center' TextWrapping='Wrap' MaxLines='2' TextTrimming='CharacterEllipsis'/>";
  if (pageHeight) {
    const auto layoutHeight = std::to_wstring(std::max(1, pageHeight - 8));
    const auto layout = listMode ?
        L"<Grid Height='" + layoutHeight + L"'><Grid.ColumnDefinitions><ColumnDefinition Width='Auto'/><ColumnDefinition Width='*'/></Grid.ColumnDefinitions>" +
            image + label + L"</Grid>" :
        L"<StackPanel Width='" + std::to_wstring(std::max(1, iconSize + 22)) +
            L"' MaxHeight='" + layoutHeight + L"' VerticalAlignment='Top' Padding='0,3'>" + image + label + L"</StackPanel>";
    return ux::Markup::XamlReader::Load(L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
        L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>" + layout + L"</DataTemplate>").as<ux::DataTemplate>();
  }
  const auto content = listMode ?
      L"<Grid MinHeight='30' Padding='8,0'><Grid.ColumnDefinitions><ColumnDefinition Width='Auto'/><ColumnDefinition Width='*'/></Grid.ColumnDefinitions>" +
          image + label + L"</Grid>" :
      L"<StackPanel Width='" + std::to_wstring(std::max(76, iconSize + 26)) +
          L"' Padding='4,3' Spacing='0'>" + image + label + L"</StackPanel>";
  return ux::Markup::XamlReader::Load(L"<DataTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
      L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>" + content + L"</DataTemplate>").as<ux::DataTemplate>();
}

ux::Media::ImageSource imageSource(IWICImagingFactory *factory, HICON icon) {
  if (!factory || !icon) return nullptr;
  ::winrt::com_ptr<IWICBitmap> bitmap;
  ::winrt::com_ptr<IWICFormatConverter> converter;
  UINT width = 0, height = 0;
  if (FAILED(factory->CreateBitmapFromHICON(icon, bitmap.put())) ||
      FAILED(bitmap->GetSize(&width, &height)) || !width || !height || width > 1024 || height > 1024 ||
      FAILED(factory->CreateFormatConverter(converter.put())) ||
      FAILED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
  std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
  if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) return nullptr;
  ::winrt::Windows::Storage::Streams::DataWriter writer;
  writer.WriteBytes(pixels);
  auto source = ux::Media::Imaging::SoftwareBitmapSource{};
  auto image = imaging::SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(), imaging::BitmapPixelFormat::Bgra8,
      static_cast<int>(width), static_cast<int>(height), imaging::BitmapAlphaMode::Premultiplied);
  source.SetBitmapAsync(image);
  return source;
}

}

struct Runtime::State {
  using PreTranslate = BOOL (WINAPI *)(MSG *);
  HMODULE bootstrap = nullptr;
  decltype(&MddBootstrapShutdown) shutdown = nullptr;
  dispatch::DispatcherQueueController queue{nullptr};
  ux::Hosting::WindowsXamlManager manager{nullptr};
  ::winrt::com_ptr<IslandApplication> application;
  PreTranslate preTranslate = nullptr;
  bool ready = false;

  ~State() {
    try {
      if (manager) manager.Close();
      if (queue) queue.ShutdownQueue();
      application = nullptr;
    } catch (...) {}
    if (shutdown) shutdown();
    if (bootstrap) FreeLibrary(bootstrap);
  }
};

Runtime::Runtime() : state(std::make_unique<State>()) {
  try {
    const auto dll = fn::modulePath().parent_path() / L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
    state->bootstrap = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!state->bootstrap) return;
    const auto initialize = reinterpret_cast<decltype(&MddBootstrapInitialize2)>(
        GetProcAddress(state->bootstrap, "MddBootstrapInitialize2"));
    const auto shutdown = reinterpret_cast<decltype(&MddBootstrapShutdown)>(
        GetProcAddress(state->bootstrap, "MddBootstrapShutdown"));
    if (!initialize || !shutdown) return;
    ::winrt::check_hresult(initialize(0x00010008, nullptr, PACKAGE_VERSION{},
                                     MddBootstrapInitializeOptions_OnPackageIdentity_NOOP));
    state->shutdown = shutdown;
    if (!dispatch::DispatcherQueue::GetForCurrentThread())
      state->queue = dispatch::DispatcherQueueController::CreateOnCurrentThread();
    state->application = ::winrt::make_self<IslandApplication>();
    state->manager = ux::Hosting::WindowsXamlManager::InitializeForCurrentThread();
    state->application->Resources().MergedDictionaries().Append(controls::XamlControlsResources{});
    if (const auto windowing = GetModuleHandleW(L"Microsoft.UI.Windowing.Core.dll"))
      state->preTranslate = reinterpret_cast<State::PreTranslate>(GetProcAddress(windowing, "ContentPreTranslateMessage"));
    state->ready = true;
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"initialize", error);
  }
}

Runtime::~Runtime() = default;
bool Runtime::available() const { return state->ready; }
bool Runtime::preTranslate(MSG &message) const {
  return state->preTranslate && state->preTranslate(&message);
}

struct Surface::State {
  double headerHeight = 36;
  TitleStyle titleStyle = TitleStyle::Text;
  ux::Media::ImageSource titleImage{nullptr};
  HICON cachedIcon = nullptr;
  controls::StackPanel titleContent{nullptr};
  controls::Image titleIcon{nullptr};
  controls::Image firstTabIcon{nullptr};
  static constexpr double headerPadding = 6;
  static constexpr double buttonWidth = 30;
  HWND parent = nullptr;
  HWND islandWindow = nullptr;
  Events events;
  ux::Hosting::DesktopWindowXamlSource island{nullptr};
  controls::Grid root{nullptr};
  ux::IUIElementProtected rootCursor{nullptr};
  LRESULT resizeCursorHit = HTCLIENT;
  ux::Media::SolidColorBrush backgroundTint{nullptr};
  ux::Shapes::Path outline{nullptr};
  ux::Media::RectangleGeometry outlineBounds{nullptr};
  ux::Media::PathFigure outlineInterior{nullptr};
  std::array<ux::Media::LineSegment, 4> outlineSides{nullptr, nullptr, nullptr, nullptr};
  std::array<ux::Media::ArcSegment, 4> outlineCorners{nullptr, nullptr, nullptr, nullptr};
  controls::Canvas resizeGrip{nullptr};
  std::array<ux::Shapes::Line, 2> resizeLines{nullptr, nullptr};
  controls::Canvas titleBar{nullptr};
  controls::TextBlock title{nullptr};
  controls::Grid body{nullptr};
  controls::StackPanel pageDots{nullptr};
  std::function<void(int)> navigatePage;
  int page = 0, pageCount = 0, firstDot = -1;
  struct PageDot {
    controls::Border target;
    ux::Shapes::Ellipse shape;
  };
  std::vector<PageDot> dots;
  PageMetrics gridPageMetrics;
  int listPageHeight = 0;
  int templateIconSize = -1;
  int templatePageHeight = 0, spacing = 0;
  controls::ItemsPanelTemplate defaultGridPanel{nullptr};
  controls::ItemsPanelTemplate fixedGridPanel{nullptr};
  ux::Media::Animation::TransitionCollection reflowTransitions{nullptr};
  ux::Media::Animation::Storyboard zoomAnimation{nullptr};
  dispatch::DispatcherQueueTimer reflowTimer{nullptr};
  bool resizing = false;
  controls::Border selectionFrame{nullptr};
  ux::Input::Pointer selectionPointer{nullptr};
  Point selectionOrigin{};
  std::vector<int> selectionBeforeDrag;
  bool selecting = false, toggleSelection = false, extendSelection = false;
  ::winrt::com_ptr<BoxAcrylic> acrylic;
  bool acrylicAttached = false;
  struct HeaderControl {
    controls::Border box{nullptr};
    controls::TextBlock label{nullptr};
    ux::Media::SolidColorBrush iconStroke{nullptr};
    double left = 0;
    double width = 0;
  };
  std::array<HeaderControl, 3> buttons;
  std::vector<HeaderControl> tabs;
  std::vector<std::wstring> titles;
  std::size_t activeTab = 0;
  std::optional<std::size_t> hoveredTab, pressedTab;
  POINT tabPress{};
  dispatch::DispatcherQueueTimer tabHover{nullptr};
  COLORREF textColor{}, mutedColor{}, borderColor{}, accentColor{};
  ux::Media::SolidColorBrush selectedFill{nullptr}, selectedHoverFill{nullptr}, selectedPressedFill{nullptr};
  bool actionsVisible = true, collapsed = false, highlighted = false, highContrast = false;
  int cornerRadius = 0;
  std::function<void()> frameCallback;
  ::winrt::event_token frameToken{};
  controls::ListViewBase view{nullptr};
  controls::ListViewBase listView{nullptr}, gridView{nullptr};
  ::winrt::com_ptr<IWICImagingFactory> factory;
  struct Row {
    std::wstring name;
    ux::Media::ImageSource image{nullptr};
  };
  std::vector<Row> items;
  int size = 0;
  bool details = false, synchronizing = false;

  ~State() {
    try { if (frameToken) ux::Media::CompositionTarget::Rendering(frameToken); } catch (...) {}
    try { if (tabHover) tabHover.Stop(); } catch (...) {}
    try { if (reflowTimer) reflowTimer.Stop(); } catch (...) {}
    try { if (zoomAnimation) zoomAnimation.Stop(); } catch (...) {}
    if (islandWindow) RemoveWindowSubclass(islandWindow, islandInput, 1);
    try { if (acrylicAttached) island.SystemBackdrop(nullptr); } catch (...) {}
    try { if (island) island.Close(); } catch (...) {}
  }

  [[nodiscard]] bool headerInteractive(POINT screen) const {
    if (!islandWindow || !ScreenToClient(islandWindow, &screen)) return false;
    const double px = GetDpiForWindow(islandWindow) / 96.0;
    if (px <= 0) return false;
    const double x = screen.x / px, y = screen.y / px;
    if (y < headerPadding || y >= headerHeight) return false;
    const auto hit = [x](const auto &control) {
      return x >= control.left && x < control.left + control.width;
    };
    if (std::any_of(tabs.begin(), tabs.end(), hit)) return true;
    return y < headerHeight - headerPadding &&
           ((actionsVisible && (hit(buttons[0]) || hit(buttons[1]))) || hit(buttons[2]));
  }

  LRESULT resizeHit(POINT point) const {
    const auto hit = SendMessageW(parent, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
    return hit == titleHeightHit || (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) ? hit : HTCLIENT;
  }

  void updateResizeCursor(LRESULT hit) {
    if (!rootCursor || hit == resizeCursorHit) return;
    using namespace ::winrt::Microsoft::UI::Input;
    std::optional<InputSystemCursorShape> shape;
    switch (hit) {
    case HTLEFT: case HTRIGHT: shape = InputSystemCursorShape::SizeWestEast; break;
    case HTTOP: case HTBOTTOM: case titleHeightHit: shape = InputSystemCursorShape::SizeNorthSouth; break;
    case HTTOPLEFT: case HTBOTTOMRIGHT: shape = InputSystemCursorShape::SizeNorthwestSoutheast; break;
    case HTTOPRIGHT: case HTBOTTOMLEFT: shape = InputSystemCursorShape::SizeNortheastSouthwest; break;
    }
    try {
      if (shape) rootCursor.ProtectedCursor(InputSystemCursor::Create(*shape));
      else rootCursor.ProtectedCursor(nullptr);
      resizeCursorHit = hit;
    } catch (const ::winrt::hresult_error &error) {
      logFailure(L"set box resize cursor", error);
    }
  }

  void trackResizeCursor(const ux::Input::PointerRoutedEventArgs &args) {
    namespace input = ::winrt::Microsoft::UI::Input;
    if (args.Pointer().PointerDeviceType() != input::PointerDeviceType::Mouse) return;
    POINT point{};
    const bool tracking = !selectionPointer && GetCursorPos(&point);
    updateResizeCursor(tracking ? resizeHit(point) : HTCLIENT);
  }

  static LRESULT CALLBACK islandInput(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    const auto &state = *reinterpret_cast<const State *>(data);
    if (message == WM_NCHITTEST) {
      const auto hit = SendMessageW(state.parent, WM_NCHITTEST, 0, l);
      if (hit == titleHeightHit || (hit >= HTLEFT && hit <= HTBOTTOMRIGHT)) return HTCLIENT;
      if (hit == HTCAPTION) return HTTRANSPARENT;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_SETCURSOR) {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      const bool havePoint = message == WM_SETCURSOR ? GetCursorPos(&point) : ClientToScreen(window, &point);
      if (havePoint) {
        const auto screen = MAKELPARAM(point.x, point.y);
        const auto hit = state.resizeHit(point);
        if (hit != HTCLIENT) {
          if (message == WM_SETCURSOR) {
            SendMessageW(state.parent, WM_SETCURSOR, reinterpret_cast<WPARAM>(state.parent),
                         MAKELPARAM(hit, HIWORD(l)));
          } else {
            SendMessageW(state.parent, WM_NCLBUTTONDOWN, hit, screen);
          }
          return TRUE;
        }
      }
    }
    return DefSubclassProc(window, message, w, l);
  }

  HeaderControl makeHeaderControl(const wchar_t *description) {
    HeaderControl control{controls::Border{}, controls::TextBlock{}};
    control.box.Background(solidBrush(0, 0));
    control.box.IsTabStop(true);
    control.box.UseSystemFocusVisuals(true);
    control.label.FontSize(13);
    control.label.VerticalAlignment(ux::VerticalAlignment::Center);
    control.label.TextTrimming(ux::TextTrimming::CharacterEllipsis);
    ux::Automation::AutomationProperties::SetName(control.box, description);
    titleBar.Children().Append(control.box);
    return control;
  }

  void makeViewIcon(HeaderControl &control, bool grid) {
    controls::Canvas icon;
    icon.Width(16);
    icon.Height(16);
    icon.HorizontalAlignment(ux::HorizontalAlignment::Center);
    icon.VerticalAlignment(ux::VerticalAlignment::Center);
    icon.IsHitTestVisible(false);
    control.iconStroke = solidBrush(mutedColor);
    constexpr double scale = 16.0 / 24;
    const auto paint = [&](const ux::Shapes::Shape &shape) {
      shape.Stroke(control.iconStroke);
      shape.StrokeThickness(2 * scale);
      icon.Children().Append(shape);
    };
    if (grid) {
      for (const int y : icons::gridStarts) for (const int x : icons::gridStarts) {
        ux::Shapes::Rectangle rectangle;
        rectangle.Width(7 * scale);
        rectangle.Height(7 * scale);
        rectangle.RadiusX(scale);
        rectangle.RadiusY(scale);
        controls::Canvas::SetLeft(rectangle, x * scale);
        controls::Canvas::SetTop(rectangle, y * scale);
        paint(rectangle);
      }
    } else {
      for (const int y : icons::listRows) for (const auto x : {3, 8}) {
        ux::Shapes::Line line;
        line.X1(x * scale);
        line.X2((x == 3 ? 3.01 : 21) * scale);
        line.Y1(y * scale);
        line.Y2(y * scale);
        line.StrokeStartLineCap(ux::Media::PenLineCap::Round);
        line.StrokeEndLineCap(ux::Media::PenLineCap::Round);
        paint(line);
      }
    }
    control.box.Child(icon);
  }

  void paintHeaderButton(HeaderControl &button, COLORREF color) {
    if (button.iconStroke) button.iconStroke.Color(windowsColor(color));
    else button.label.Foreground(solidBrush(color));
  }

  void makeHeader() {
    title = controls::TextBlock{};
    title.FontWeight({600});
    title.FontSize(14);
    title.TextTrimming(ux::TextTrimming::CharacterEllipsis);
    title.IsHitTestVisible(false);
    titleContent = controls::StackPanel{};
    titleContent.Orientation(controls::Orientation::Horizontal);
    titleContent.Spacing(7);
    titleContent.IsHitTestVisible(false);
    titleIcon = controls::Image{};
    titleIcon.Width(18);
    titleIcon.Height(18);
    titleIcon.VerticalAlignment(ux::VerticalAlignment::Center);
    titleContent.Children().Append(titleIcon);
    titleContent.Children().Append(title);
    controls::Canvas::SetLeft(titleContent, 14);
    titleBar.Children().Append(titleContent);
    tabHover = dispatch::DispatcherQueue::GetForCurrentThread().CreateTimer();
    tabHover.Interval(std::chrono::milliseconds(140));
    tabHover.IsRepeating(false);
    tabHover.Tick([this](auto const &, auto const &) {
      if (hoveredTab && !pressedTab && events.tab) events.tab(*hoveredTab);
    });
    const std::array labels{lang::text(L"图标"), lang::text(L"列表"), lang::text(L"折叠盒子")};
    for (int i = 0; i < static_cast<int>(buttons.size()); ++i) {
      auto control = makeHeaderControl(labels[i]);
      if (i < 2) makeViewIcon(control, i == 0);
      else {
        control.label.Text(L"\uE70E");
        control.label.FontFamily(ux::Media::FontFamily(L"Segoe MDL2 Assets"));
        control.label.HorizontalAlignment(ux::HorizontalAlignment::Center);
        control.box.Child(control.label);
      }
      control.box.PointerEntered([this, i](auto const &, auto const &) { paintHeaderButton(buttons[i], textColor); });
      control.box.PointerExited([this, i](auto const &, auto const &) { paintHeaderButton(buttons[i], mutedColor); });
      control.box.Tapped([this, i](auto const &, ux::Input::TappedRoutedEventArgs const &args) {
        args.Handled(true);
        if (events.headerCommand) events.headerCommand(i);
      });
      control.box.KeyDown([this, i](auto const &, ux::Input::KeyRoutedEventArgs const &args) {
        const auto key = args.Key();
        if ((key == ::winrt::Windows::System::VirtualKey::Enter || key == ::winrt::Windows::System::VirtualKey::Space) &&
            events.headerCommand) {
          args.Handled(true);
          events.headerCommand(i);
        }
      });
      buttons[i] = std::move(control);
    }
  }

  void makeTabs() {
    tabHover.Stop();
    hoveredTab.reset();
    pressedTab.reset();
    for (const auto &tab : tabs) {
      uint32_t index{};
      if (titleBar.Children().IndexOf(tab.box, index)) titleBar.Children().RemoveAt(index);
    }
    tabs.clear();
    firstTabIcon = nullptr;
    if (titles.size() < 2) return;
    for (std::size_t index = 0; index < titles.size(); ++index) {
      auto control = makeHeaderControl(titles[index].c_str());
      control.box.CornerRadius({5, 5, 0, 0});
      control.box.Padding({8, 0, 8, 0});
      control.label.Text(titles[index]);
      control.label.TextAlignment(ux::TextAlignment::Center);
      if (index == 0 && titleStyle != TitleStyle::Text) {
        controls::StackPanel stack;
        stack.Orientation(controls::Orientation::Horizontal);
        stack.HorizontalAlignment(ux::HorizontalAlignment::Center);
        stack.VerticalAlignment(ux::VerticalAlignment::Center);
        stack.Spacing(7);
        stack.IsHitTestVisible(false);
        firstTabIcon = controls::Image{};
        firstTabIcon.Width(17);
        firstTabIcon.Height(17);
        stack.Children().Append(firstTabIcon);
        if (titleStyle == TitleStyle::IconAndText) stack.Children().Append(control.label);
        control.box.Child(stack);
      } else {
        control.box.Child(control.label);
      }
      control.box.PointerEntered([this, index](auto const &, auto const &) {
        hoveredTab = index;
        paintTabs();
        if (index != activeTab && !pressedTab) tabHover.Start();
      });
      control.box.PointerExited([this, index](auto const &, auto const &) {
        if (hoveredTab == index) {
          tabHover.Stop();
          hoveredTab.reset();
          paintTabs();
        }
      });
      control.box.PointerPressed([this, index](auto const &sender, ux::Input::PointerRoutedEventArgs const &args) {
        const auto tab = sender.template as<controls::Border>();
        if (!args.GetCurrentPoint(tab).Properties().IsLeftButtonPressed()) return;
        tabHover.Stop();
        tab.Focus(ux::FocusState::Pointer);
        if (GetCursorPos(&tabPress) && tab.CapturePointer(args.Pointer())) pressedTab = index;
        args.Handled(true);
      });
      control.box.PointerReleased([this, index](auto const &sender, ux::Input::PointerRoutedEventArgs const &args) {
        if (pressedTab != index) return;
        pressedTab.reset();
        const auto tab = sender.template as<controls::Border>();
        tab.ReleasePointerCapture(args.Pointer());
        POINT end{};
        args.Handled(true);
        if (index && events.tabDropped && GetCursorPos(&end)) events.tabDropped(index, tabPress, end);
        else if (events.tab) events.tab(index);
      });
      control.box.PointerCaptureLost([this, index](auto const &, auto const &) {
        if (pressedTab == index) pressedTab.reset();
      });
      control.box.KeyDown([this, index](auto const &, ux::Input::KeyRoutedEventArgs const &args) {
        using Key = ::winrt::Windows::System::VirtualKey;
        auto next = index;
        if (args.Key() == Key::Left) next = index ? index - 1 : tabs.size() - 1;
        else if (args.Key() == Key::Right) next = (index + 1) % tabs.size();
        else if (args.Key() != Key::Space && args.Key() != Key::Enter) return;
        args.Handled(true);
        tabs[next].box.Focus(ux::FocusState::Keyboard);
        if (events.tab) events.tab(next);
      });
      tabs.push_back(std::move(control));
    }
  }

  void paintTabs() {
    for (std::size_t index = 0; index < tabs.size(); ++index) {
      const bool selected = index == activeTab;
      const bool hot = hoveredTab == index;
      auto &tab = tabs[index];
      tab.label.Foreground(solidBrush(selected || hot ? textColor : mutedColor));
      tab.box.Background(solidBrush(highContrast && selected ? accentColor : mutedColor,
                                    selected ? (highContrast ? 255 : 44) : hot ? 26 : 0));
    }
  }

  void positionHeader() {
    RECT area{};
    GetClientRect(islandWindow, &area);
    const double scale = GetDpiForWindow(islandWindow) / 96.0;
    if (scale <= 0) return;
    const double width = area.right / scale;
    const auto place = [](HeaderControl &control, double left, double width, double height) {
      control.left = left;
      control.width = width;
      control.box.Width(width);
      control.box.Height(height);
      controls::Canvas::SetLeft(control.box, left);
      controls::Canvas::SetTop(control.box, headerPadding);
    };
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      place(buttons[i], std::max(0.0, width - headerPadding - buttonWidth * (buttons.size() - i)),
            buttonWidth, headerHeight - 2 * headerPadding);
    }
    if (!tabs.empty()) {
      const double space = std::max(0.0, width - 2 * headerPadding - buttonWidth * buttons.size() - 16);
      const double tabWidth = std::min(116.0, space / tabs.size());
      for (std::size_t i = 0; i < tabs.size(); ++i)
        place(tabs[i], headerPadding + i * tabWidth, tabWidth, headerHeight - headerPadding);
      if (titleStyle == TitleStyle::IconAndText)
        tabs.front().label.MaxWidth(std::max(0.0, tabWidth - 40));
    }
    const double titleSpace = std::max(0.0, width - (actionsVisible ? 3 : 1) * buttonWidth - 24);
    title.Width(std::max(0.0, titleSpace - (titleStyle == TitleStyle::Text ? 0 : 25)));
    controls::Canvas::SetTop(titleContent, (headerHeight - 20) / 2);
  }

  void showActions(bool visible) {
    if (actionsVisible == visible) return;
    actionsVisible = visible;
    for (int i = 0; i < 2; ++i) {
      buttons[i].box.Visibility(visible ? ux::Visibility::Visible : ux::Visibility::Collapsed);
      paintHeaderButton(buttons[i], mutedColor);
    }
    positionHeader();
  }

  void makeOutline() {
    outline = ux::Shapes::Path{};
    outline.IsHitTestVisible(false);
    outline.Stretch(ux::Media::Stretch::None);
    outlineBounds = ux::Media::RectangleGeometry{};
    outlineInterior = ux::Media::PathFigure{};
    outlineInterior.IsClosed(true);
    for (std::size_t i = 0; i < outlineSides.size(); ++i) {
      auto &side = outlineSides[i];
      auto &corner = outlineCorners[i];
      side = ux::Media::LineSegment{};
      corner = ux::Media::ArcSegment{};
      corner.SweepDirection(ux::Media::SweepDirection::Clockwise);
      outlineInterior.Segments().Append(side);
      outlineInterior.Segments().Append(corner);
    }
    ux::Media::PathGeometry interior;
    interior.Figures().Append(outlineInterior);
    ux::Media::GeometryGroup frame;
    frame.FillRule(ux::Media::FillRule::EvenOdd);
    frame.Children().Append(outlineBounds);
    frame.Children().Append(interior);
    outline.Data(frame);
    controls::Canvas layer;
    layer.IsHitTestVisible(false);
    layer.Children().Append(outline);
    root.Children().Append(layer);
  }

  void positionOutline() {
    RECT area{};
    const auto dpi = GetDpiForWindow(parent);
    if (!dpi || !GetClientRect(parent, &area)) return;
    const auto width = area.right - area.left, height = area.bottom - area.top;
    if (width <= 0 || height <= 0) return;
    const auto diameter = std::min({static_cast<LONG>(MulDiv(cornerRadius * 2, dpi, 96)), width, height});
    const float pixel = 96.0f / dpi;
    const float thickness = (highlighted ? 2 : 1) * pixel;
    // The native window region defines the outer edge. Fill through that edge so no strip of tint
    // remains between the stroke and the silhouette, even when DPI rounding shifts the corner.
    controls::Canvas::SetLeft(outline, -pixel);
    controls::Canvas::SetTop(outline, -pixel);
    outlineBounds.Rect({0, 0, width * pixel + 2 * pixel, height * pixel + 2 * pixel});
    const float left = pixel + thickness, top = left;
    const float right = std::max(left, (width + 1) * pixel - thickness);
    const float bottom = std::max(top, (height + 1) * pixel - thickness);
    const float radius = std::clamp((diameter / 2.0f - 0.5f) * pixel - (highlighted ? pixel : 0), 0.0f,
                                   std::min(right - left, bottom - top) / 2);
    outlineInterior.StartPoint({left + radius, top});
    const std::array<Point, 4> sides{{{right - radius, top}, {right, bottom - radius},
                                      {left + radius, bottom}, {left, top + radius}}};
    const std::array<Point, 4> corners{{{right, top + radius}, {right - radius, bottom},
                                        {left, bottom - radius}, {left + radius, top}}};
    for (std::size_t i = 0; i < outlineSides.size(); ++i) {
      outlineSides[i].Point(sides[i]);
      outlineCorners[i].Size({radius, radius});
      outlineCorners[i].Point(corners[i]);
    }
  }

  void paintOutline() {
    outline.Fill(solidBrush(highlighted ? accentColor : highContrast ? borderColor : textColor,
                            highlighted || highContrast ? 255 : boxOutlineAlpha));
    positionOutline();
  }

  void positionBody() {
    const auto bottom = static_cast<double>(std::max(10, cornerRadius));
    body.Margin({contentLeftPadding, headerHeight + contentTopPadding, contentRightPadding,
                 bottom + (pageCount > 0 ? pageFooterHeight : 0)});
    pageDots.Margin({0, 0, 0, bottom});
    pageDots.Visibility(pageCount > 0 && !collapsed ? ux::Visibility::Visible : ux::Visibility::Collapsed);
  }

  void paintPageDot(const PageDot &dot, bool active, bool hover) {
    dot.shape.Width(active ? 7 : 5);
    dot.shape.Height(active ? 7 : 5);
    dot.shape.Fill(solidBrush(active ? textColor : mutedColor, active || hover || highContrast ? 255 : 160));
  }

  void paintSelectedItems() {
    selectedFill.Color(windowsColor(textColor, 32));
    selectedHoverFill.Color(windowsColor(textColor, 42));
    selectedPressedFill.Color(windowsColor(textColor, 49));
  }

  void styleItem(const controls::ContentControl &container, bool listMode) const {
    const int gap = listMode && listPageHeight ? std::min(spacing, (listPageHeight - 1) / 2) : spacing;
    const auto extra = gap / 2.0;
    const auto baseline = listMode ? 0.0 : 4.0;
    const auto margin = ux::Thickness{extra, extra, extra + baseline, extra + baseline};
    if (const auto current = container.Margin(); current.Left != margin.Left || current.Top != margin.Top ||
        current.Right != margin.Right || current.Bottom != margin.Bottom) container.Margin(margin);
    if (listMode && listPageHeight) {
      const auto height = listPageHeight - gap;
      if (container.MinHeight() != 0) container.MinHeight(0);
      if (container.Height() != height) container.Height(height);
    } else if (listMode) {
      container.ClearValue(ux::FrameworkElement::HeightProperty());
      container.ClearValue(ux::FrameworkElement::MinHeightProperty());
    }
    const auto presenter = visualChild<controls::Primitives::ListViewItemPresenter>(container);
    if (!presenter) return;
    if (highContrast) {
      using Presenter = controls::Primitives::ListViewItemPresenter;
      for (const auto &property : {Presenter::SelectedBorderThicknessProperty(), Presenter::SelectionIndicatorVisualEnabledProperty(),
          Presenter::SelectedBackgroundProperty(), Presenter::SelectedPointerOverBackgroundProperty(),
          Presenter::SelectedPressedBackgroundProperty()}) presenter.ClearValue(property);
      return;
    }
    if (listMode) presenter.SelectionIndicatorVisualEnabled(false);
    else presenter.SelectedBorderThickness({});
    presenter.SelectedBackground(selectedFill);
    presenter.SelectedPointerOverBackground(selectedHoverFill);
    presenter.SelectedPressedBackground(selectedPressedFill);
  }

  void updateRealizedItems() const {
    if (!view) return;
    const auto panel = view.ItemsPanelRoot();
    if (!panel) return;
    for (const auto &child : panel.Children()) {
      if (auto container = child.try_as<controls::ContentControl>()) styleItem(container, details);
    }
  }

  void configurePageGrid() const {
    if (!view || details) return;
    const auto grid = gridView.ItemsPanelRoot().try_as<controls::ItemsWrapGrid>();
    if (!grid) return;
    if (fixedGridPanel) {
      if (grid.ItemWidth() != gridPageMetrics.width) grid.ItemWidth(gridPageMetrics.width);
      if (grid.ItemHeight() != gridPageMetrics.height) grid.ItemHeight(gridPageMetrics.height);
      if (grid.MaximumRowsOrColumns() != gridPageMetrics.columns) grid.MaximumRowsOrColumns(gridPageMetrics.columns);
    }
  }

  void setReflow(bool enabled) {
    if (!gridView) return;
    if (reflowTimer) reflowTimer.Stop();
    if (enabled) {
      if (resizing) return;
      resizing = true;
      if (!reflowTransitions) {
        ux::Media::Animation::RepositionThemeTransition reposition;
        reposition.IsStaggeringEnabled(false);
        reflowTransitions = ux::Media::Animation::TransitionCollection{};
        reflowTransitions.Append(reposition);
      }
      gridView.ItemContainerTransitions(reflowTransitions);
    } else if (resizing) {
      if (!reflowTimer) {
        reflowTimer = root.DispatcherQueue().CreateTimer();
        reflowTimer.Interval(std::chrono::milliseconds(250));
        reflowTimer.IsRepeating(false);
        reflowTimer.Tick([this](auto const &, auto const &) {
          resizing = false;
          gridView.ItemContainerTransitions(ux::Media::Animation::TransitionCollection{});
        });
      }
      reflowTimer.Start();
    }
  }

  void positionPageDots() {
    if (pageCount <= 0) return;
    RECT area{};
    const auto dpi = GetDpiForWindow(parent);
    if (!dpi || !GetClientRect(parent, &area)) return;
    constexpr int dotWidth = 20;
    const auto visible = std::clamp(static_cast<int>((area.right * 96.0 / dpi - 48) / dotWidth),
                                   1, std::min(25, pageCount));
    const auto first = std::clamp(page - visible / 2, 0, pageCount - visible);
    if (first != firstDot || visible != static_cast<int>(dots.size())) {
      pageDots.Children().Clear();
      dots.clear();
      firstDot = first;
      for (int index = first; index < first + visible; ++index) {
        PageDot dot{controls::Border{}, ux::Shapes::Ellipse{}};
        dot.target.Width(dotWidth);
        dot.target.Height(pageFooterHeight);
        dot.target.Background(solidBrush(0, 0));
        dot.target.IsTabStop(true);
        dot.target.UseSystemFocusVisuals(true);
        dot.shape.HorizontalAlignment(ux::HorizontalAlignment::Center);
        dot.shape.VerticalAlignment(ux::VerticalAlignment::Center);
        dot.target.Child(dot.shape);
        dot.target.PointerEntered([this, index](auto const &, auto const &) {
          if (index >= firstDot && index - firstDot < static_cast<int>(dots.size()))
            paintPageDot(dots[index - firstDot], index == page, true);
        });
        dot.target.PointerExited([this, index](auto const &, auto const &) {
          if (index >= firstDot && index - firstDot < static_cast<int>(dots.size()))
            paintPageDot(dots[index - firstDot], index == page, false);
        });
        dot.target.Tapped([this, index](auto const &, const ux::Input::TappedRoutedEventArgs &args) {
          args.Handled(true);
          if (auto navigate = navigatePage) navigate(index);
        });
        dot.target.KeyDown([this, index](auto const &, const ux::Input::KeyRoutedEventArgs &args) {
          using ::winrt::Windows::System::VirtualKey;
          int next = index;
          switch (args.Key()) {
          case VirtualKey::Left: next = std::max(0, index - 1); break;
          case VirtualKey::Right: next = std::min(pageCount - 1, index + 1); break;
          case VirtualKey::Enter: case VirtualKey::Space: break;
          default: return;
          }
          args.Handled(true);
          if (auto navigate = navigatePage) navigate(next);
          if (next >= firstDot && next - firstDot < static_cast<int>(dots.size()))
            dots[next - firstDot].target.Focus(ux::FocusState::Keyboard);
        });
        ux::Automation::AutomationProperties::SetName(dot.target,
            lang::format(L"第 {0} 页", {std::to_wstring(index + 1)}));
        dots.push_back(dot);
        pageDots.Children().Append(dot.target);
      }
    }
    for (int i = 0; i < visible; ++i) {
      paintPageDot(dots[i], first + i == page, false);
      ux::Automation::AutomationProperties::SetItemStatus(dots[i].target,
          std::to_wstring(first + i + 1) + L" / " + std::to_wstring(pageCount));
    }
  }

  [[nodiscard]] int hit(POINT screen) const {
    if (!view || !islandWindow || !ScreenToClient(islandWindow, &screen)) return -1;
    const float scale = GetDpiForWindow(islandWindow) / 96.0f;
    if (scale <= 0) return -1;
    const Point point{screen.x / scale, screen.y / scale};
    for (const auto &node : ux::Media::VisualTreeHelper::FindElementsInHostCoordinates(point, view)) {
      auto container = node.try_as<controls::ContentControl>();
      if (!container) continue;
      if (const int row = view.IndexFromContainer(container); row >= 0) return row;
    }
    return -1;
  }

  void apply(controls::ContentControl const &container, int index) const {
    if (!container || index < 0 || static_cast<size_t>(index) >= items.size()) return;
    auto element = container.ContentTemplateRoot().try_as<ux::FrameworkElement>();
    if (!element) return;
    const auto &item = items[index];
    if (auto text = element.FindName(L"FileName").try_as<controls::TextBlock>()) text.Text(item.name);
    if (auto image = element.FindName(L"FileImage").try_as<controls::Image>()) image.Source(item.image);
  }

  std::vector<int> selectedRows() const {
    std::vector<int> selected;
    for (const auto &item : view.SelectedItems()) selected.push_back(::winrt::unbox_value<int>(item));
    std::sort(selected.begin(), selected.end());
    return selected;
  }

  void setSelectedRows(const std::vector<int> &rows) {
    const auto current = selectedRows();
    if (rows == current) return;
    const bool previous = std::exchange(synchronizing, true);
    try {
      auto selected = view.SelectedItems();
      for (uint32_t i = selected.Size(); i > 0; --i) {
        const auto row = ::winrt::unbox_value<int>(selected.GetAt(i - 1));
        if (!std::binary_search(rows.begin(), rows.end(), row)) selected.RemoveAt(i - 1);
      }
      for (const auto row : rows) {
        if (!std::binary_search(current.begin(), current.end(), row)) selected.Append(view.Items().GetAt(row));
      }
    } catch (...) {
      synchronizing = previous;
      throw;
    }
    synchronizing = previous;
    if (!synchronizing && events.selection) events.selection(rows);
  }

  void beginSelection(const ux::Input::PointerRoutedEventArgs &args) {
    const auto pointer = args.Pointer();
    if (selectionPointer || collapsed || pointer.PointerDeviceType() != ::winrt::Microsoft::UI::Input::PointerDeviceType::Mouse) return;
    const auto point = args.GetCurrentPoint(body);
    if (!point.Properties().IsLeftButtonPressed()) return;
    POINT screen{};
    if (!GetCursorPos(&screen) || resizeHit(screen) != HTCLIENT) return;
    for (auto node = args.OriginalSource().try_as<ux::DependencyObject>(); node && node != body;
         node = ux::Media::VisualTreeHelper::GetParent(node)) {
      if (node.try_as<controls::Primitives::ScrollBar>() || node.try_as<controls::Primitives::SelectorItem>()) return;
    }
    if (!body.CapturePointer(pointer)) return;
    selectionPointer = pointer;
    selectionOrigin = point.Position();
    selectionBeforeDrag = selectedRows();
    toggleSelection = GetKeyState(VK_CONTROL) < 0;
    extendSelection = GetKeyState(VK_SHIFT) < 0;
    view.Focus(ux::FocusState::Pointer);
    args.Handled(true);
  }

  void updateSelection(Point position) {
    if (!selecting) {
      const float scale = GetDpiForWindow(islandWindow) / 96.0f;
      if (std::abs(position.X - selectionOrigin.X) * scale < GetSystemMetrics(SM_CXDRAG) &&
          std::abs(position.Y - selectionOrigin.Y) * scale < GetSystemMetrics(SM_CYDRAG)) return;
      selecting = true;
      selectionFrame.Background(solidBrush(accentColor, highContrast ? 25 : 42));
      selectionFrame.BorderBrush(solidBrush(accentColor));
      selectionFrame.Visibility(ux::Visibility::Visible);
    }
    position.X = std::clamp(position.X, 0.0f, static_cast<float>(body.ActualWidth()));
    position.Y = std::clamp(position.Y, 0.0f, static_cast<float>(body.ActualHeight()));
    const Rect area{std::min(position.X, selectionOrigin.X), std::min(position.Y, selectionOrigin.Y),
                    std::abs(position.X - selectionOrigin.X), std::abs(position.Y - selectionOrigin.Y)};
    controls::Canvas::SetLeft(selectionFrame, area.X);
    controls::Canvas::SetTop(selectionFrame, area.Y);
    selectionFrame.Width(area.Width);
    selectionFrame.Height(area.Height);

    std::vector<int> hitRows;
    if (const auto itemsPanel = view.ItemsPanelRoot()) {
      for (const auto &child : itemsPanel.Children()) {
        const auto item = child.try_as<controls::Primitives::SelectorItem>();
        if (!item) continue;
        const int row = view.IndexFromContainer(item);
        if (row < 0) continue;
        const auto bounds = item.TransformToVisual(body).TransformBounds({0, 0, static_cast<float>(item.ActualWidth()),
                                                                          static_cast<float>(item.ActualHeight())});
        if (bounds.X < area.X + area.Width && bounds.X + bounds.Width > area.X &&
            bounds.Y < area.Y + area.Height && bounds.Y + bounds.Height > area.Y) hitRows.push_back(row);
      }
    }
    std::sort(hitRows.begin(), hitRows.end());
    if (!toggleSelection && !extendSelection) {
      setSelectedRows(hitRows);
      return;
    }
    std::vector<int> combined;
    if (toggleSelection) {
      std::set_symmetric_difference(selectionBeforeDrag.begin(), selectionBeforeDrag.end(),
                                    hitRows.begin(), hitRows.end(), std::back_inserter(combined));
    } else {
      std::set_union(selectionBeforeDrag.begin(), selectionBeforeDrag.end(),
                     hitRows.begin(), hitRows.end(), std::back_inserter(combined));
    }
    setSelectedRows(combined);
  }

  void endSelection(bool canceled) {
    if (!selectionPointer) return;
    const auto pointer = std::exchange(selectionPointer, nullptr);
    body.ReleasePointerCapture(pointer);
    selectionFrame.Visibility(ux::Visibility::Collapsed);
    const bool changed = std::exchange(selecting, false);
    if (canceled && changed) setSelectedRows(selectionBeforeDrag);
    if (!canceled && !changed && !toggleSelection && !extendSelection) setSelectedRows({});
    selectionBeforeDrag.clear();
  }

  void configureSelection() {
    body.Background(solidBrush(0, 0));
    controls::Canvas overlay;
    overlay.IsHitTestVisible(false);
    selectionFrame = controls::Border{};
    selectionFrame.BorderThickness({1, 1, 1, 1});
    selectionFrame.Visibility(ux::Visibility::Collapsed);
    overlay.Children().Append(selectionFrame);
    body.Children().Append(overlay);

    body.AddHandler(ux::UIElement::PointerPressedEvent(), ::winrt::box_value(ux::Input::PointerEventHandler(
        [this](auto const &, auto const &args) { beginSelection(args); })), true);
    body.AddHandler(ux::UIElement::PointerMovedEvent(), ::winrt::box_value(ux::Input::PointerEventHandler(
        [this](auto const &, const ux::Input::PointerRoutedEventArgs &args) {
          if (!selectionPointer || args.Pointer().PointerId() != selectionPointer.PointerId()) return;
          updateSelection(args.GetCurrentPoint(body).Position());
          args.Handled(true);
        })), true);
    body.AddHandler(ux::UIElement::PointerReleasedEvent(), ::winrt::box_value(ux::Input::PointerEventHandler(
        [this](auto const &, const ux::Input::PointerRoutedEventArgs &args) {
          if (!selectionPointer || args.Pointer().PointerId() != selectionPointer.PointerId()) return;
          updateSelection(args.GetCurrentPoint(body).Position());
          endSelection(false);
          args.Handled(true);
        })), true);
    const auto cancel = [this](auto const &, const ux::Input::PointerRoutedEventArgs &args) {
      if (selectionPointer && args.Pointer().PointerId() == selectionPointer.PointerId()) endSelection(true);
    };
    body.PointerCanceled(cancel);
    body.PointerCaptureLost(cancel);
    body.KeyDown([this](auto const &, const ux::Input::KeyRoutedEventArgs &args) {
      if (selectionPointer && args.Key() == ::winrt::Windows::System::VirtualKey::Escape) {
        endSelection(true);
        args.Handled(true);
      }
    });
  }

  void makeView(bool listMode, int iconSize) {
    const bool viewChanged = !view || listMode != details;
    details = listMode;
    size = iconSize;
    if (!listView || !gridView) {
      listView = controls::ListView{}.as<controls::ListViewBase>();
      gridView = controls::GridView{}.as<controls::ListViewBase>();
      for (const auto &itemsView : {listView, gridView}) {
        configureView(itemsView);
        body.Children().Append(itemsView);
      }
      defaultGridPanel = gridView.ItemsPanel();
    }
    if (viewChanged) {
      view = details ? listView : gridView;
      const auto hidden = details ? gridView : listView;
      hidden.Visibility(ux::Visibility::Collapsed);
      hidden.ItemsSource(nullptr);
      view.Visibility(ux::Visibility::Visible);
    }
    updatePageLayout(viewChanged);
  }

  void updatePageLayout(bool viewChanged = false) {
    if (!view) return;
    const auto paged = pageCount > 0;
    const auto metrics = paged ? measurePage(parent, details, size, static_cast<int>(headerHeight), cornerRadius, spacing) : PageMetrics{};
    const auto iconSize = paged ? metrics.iconSize : size;
    const auto pageHeight = paged ? metrics.height : 0;
    if (viewChanged || templateIconSize != iconSize || templatePageHeight != pageHeight) {
      templateIconSize = iconSize;
      templatePageHeight = pageHeight;
      view.ItemTemplate(itemTemplate(details, iconSize, pageHeight));
    }
    if (!paged && !spacing && fixedGridPanel) {
      gridView.ItemsPanel(defaultGridPanel);
      fixedGridPanel = nullptr;
    }
    if (!details && (paged || spacing)) {
      gridPageMetrics = paged ? metrics : PageMetrics{std::max(84, size + 34) + spacing,
                                                     size + 54 + spacing, iconSize, -1};
      if (!fixedGridPanel) {
        fixedGridPanel = ux::Markup::XamlReader::Load(
            L"<ItemsPanelTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'>"
            L"<ItemsWrapGrid Orientation='Horizontal'/></ItemsPanelTemplate>").as<controls::ItemsPanelTemplate>();
        gridView.ItemsPanel(fixedGridPanel);
      }
    }
    listPageHeight = paged && details ? metrics.height : 0;
    configurePageGrid();
    updateRealizedItems();
  }

  void configurePageScroll() {
    for (const auto &itemsView : {listView, gridView}) {
      if (!itemsView) continue;
      controls::ScrollViewer::SetVerticalScrollMode(itemsView,
          pageCount > 0 ? controls::ScrollMode::Disabled : controls::ScrollMode::Auto);
      controls::ScrollViewer::SetVerticalScrollBarVisibility(itemsView,
          pageCount > 0 ? controls::ScrollBarVisibility::Disabled : controls::ScrollBarVisibility::Auto);
    }
  }

  void configureView(const controls::ListViewBase &itemsView) {
    itemsView.Background(ux::Media::SolidColorBrush{::winrt::Windows::UI::Color{0, 0, 0, 0}});
    // Leave room to start a selection when every list row is occupied.
    itemsView.Margin({viewLeftPadding, 0, 0, 0});
    itemsView.BorderThickness({0, 0, 0, 0});
    itemsView.SelectionMode(controls::ListViewSelectionMode::Extended);
    itemsView.IsMultiSelectCheckBoxEnabled(false);
    itemsView.CanDragItems(true);
    controls::ScrollViewer::SetHorizontalScrollMode(itemsView, controls::ScrollMode::Disabled);
    controls::ScrollViewer::SetHorizontalScrollBarVisibility(itemsView, controls::ScrollBarVisibility::Disabled);
    controls::ScrollViewer::SetVerticalScrollBarVisibility(itemsView, controls::ScrollBarVisibility::Auto);
    itemsView.ContainerContentChanging([this](const controls::ListViewBase &sender, controls::ContainerContentChangingEventArgs const &args) {
      if (sender != view || args.InRecycleQueue()) return;
      const auto container = args.ItemContainer();
      styleItem(container, details);
      apply(container, args.ItemIndex());
    });
    itemsView.LayoutUpdated([this](const auto &, const auto &) { configurePageGrid(); });
    itemsView.SelectionChanged([this](auto const &sender, auto const &) {
      if (sender != view || synchronizing || !events.selection) return;
      events.selection(selectedRows());
    });
    itemsView.DoubleTapped([this](auto const &, ux::Input::DoubleTappedRoutedEventArgs const &args) {
      POINT point{};
      if (events.open && GetCursorPos(&point)) {
        const auto row = hit(point);
        if (row >= 0) events.open(row);
      }
      args.Handled(true);
    });
    itemsView.KeyDown([this](auto const &, ux::Input::KeyRoutedEventArgs const &args) {
      if (args.Key() == ::winrt::Windows::System::VirtualKey::Enter && events.open && view.SelectedIndex() >= 0) {
        events.open(view.SelectedIndex());
        args.Handled(true);
      }
    });
    itemsView.DragItemsStarting([this](auto const &, controls::DragItemsStartingEventArgs const &args) {
      args.Cancel(true);
      if (events.beginDrag && args.Items().Size()) events.beginDrag(::winrt::unbox_value<int>(args.Items().GetAt(0)));
    });
    itemsView.AddHandler(ux::UIElement::PointerWheelChangedEvent(),
        ::winrt::box_value(ux::Input::PointerEventHandler([this](auto const &, ux::Input::PointerRoutedEventArgs const &args) {
          if (!events.zoom || GetKeyState(VK_CONTROL) >= 0) return;
          args.Handled(true);
          events.zoom(args.GetCurrentPoint(view).Properties().MouseWheelDelta());
        })), true);
  }
};

Surface::Surface(HWND parent, Events events) : state(std::make_unique<State>()) {
  try {
    state->parent = parent;
    state->events = std::move(events);
    state->island = ux::Hosting::DesktopWindowXamlSource{};
    state->island.Initialize(::winrt::Microsoft::UI::GetWindowIdFromWindow(parent));
    state->root = controls::Grid{};
    state->rootCursor = state->root.try_as<ux::IUIElementProtected>();
    state->backgroundTint = solidBrush(0, 0);
    state->selectedFill = solidBrush(0, 0);
    state->selectedHoverFill = solidBrush(0, 0);
    state->selectedPressedFill = solidBrush(0, 0);
    state->root.Background(state->backgroundTint);
    const auto resizeCursor = ::winrt::box_value(ux::Input::PointerEventHandler(
        [state = state.get()](auto const &, const auto &args) { state->trackResizeCursor(args); }));
    state->root.AddHandler(ux::UIElement::PointerEnteredEvent(), resizeCursor, true);
    state->root.AddHandler(ux::UIElement::PointerMovedEvent(), resizeCursor, true);
    state->root.PointerExited([state = state.get()](auto const &, auto const &) {
      state->updateResizeCursor(HTCLIENT);
    });
    state->root.AddHandler(ux::UIElement::PointerWheelChangedEvent(),
        ::winrt::box_value(ux::Input::PointerEventHandler([state = state.get()](auto const &, const auto &args) {
          const auto point = args.GetCurrentPoint(state->root);
          if (!state->pageCount || state->collapsed || !state->events.wheel || GetKeyState(VK_CONTROL) < 0 ||
              point.Position().Y < state->headerHeight || point.Properties().IsHorizontalMouseWheel()) return;
          if (state->events.wheel(point.Properties().MouseWheelDelta())) args.Handled(true);
        })), true);
    state->root.AddHandler(ux::UIElement::PointerPressedEvent(),
        ::winrt::box_value(ux::Input::PointerEventHandler([state = state.get()](auto const &, const auto &args) {
          POINT point{};
          if (!args.GetCurrentPoint(state->root).Properties().IsLeftButtonPressed() || !GetCursorPos(&point)) return;
          const auto hit = state->resizeHit(point);
          if (hit == HTCLIENT) return;
          args.Handled(true);
          SendMessageW(state->parent, WM_NCLBUTTONDOWN, hit, MAKELPARAM(point.x, point.y));
        })), true);
    state->body = controls::Grid{};
    ux::Media::RectangleGeometry viewport;
    state->body.Clip(viewport);
    state->body.SizeChanged([viewport](auto const &, const ux::SizeChangedEventArgs &args) {
      const auto size = args.NewSize();
      viewport.Rect({0, 0, size.Width, size.Height});
    });
    state->pageDots = controls::StackPanel{};
    state->pageDots.Orientation(controls::Orientation::Horizontal);
    state->pageDots.HorizontalAlignment(ux::HorizontalAlignment::Center);
    state->pageDots.VerticalAlignment(ux::VerticalAlignment::Bottom);
    state->pageDots.Height(pageFooterHeight);
    state->positionBody();
    state->titleBar = controls::Canvas{};
    state->titleBar.Height(state->headerHeight);
    state->titleBar.VerticalAlignment(ux::VerticalAlignment::Top);
    state->root.Children().Append(state->body);
    state->root.Children().Append(state->titleBar);
    state->root.Children().Append(state->pageDots);
    state->makeOutline();
    state->resizeGrip = controls::Canvas{};
    state->resizeGrip.Width(20);
    state->resizeGrip.Height(20);
    state->resizeGrip.HorizontalAlignment(ux::HorizontalAlignment::Right);
    state->resizeGrip.VerticalAlignment(ux::VerticalAlignment::Bottom);
    state->resizeGrip.Background(solidBrush(0, 0));
    for (size_t i = 0; i < state->resizeLines.size(); ++i) {
      auto &line = state->resizeLines[i];
      line = ux::Shapes::Line{};
      const double length = 10.0 - i * 4.0;
      const double center = 10.0 + i * 4.0;
      line.X1(center - length / 2);
      line.Y1(center + length / 2);
      line.X2(center + length / 2);
      line.Y2(center - length / 2);
      line.StrokeThickness(1);
      line.IsHitTestVisible(false);
      state->resizeGrip.Children().Append(line);
    }
    state->root.Children().Append(state->resizeGrip);
    state->root.RightTapped([state = state.get()](auto const &, ux::Input::RightTappedRoutedEventArgs const &args) {
      POINT point{};
      if (state->events.context && GetCursorPos(&point)) state->events.context(point);
      args.Handled(true);
    });
    state->titleBar.Background(solidBrush(0, 0));
    state->titleBar.PointerPressed([state = state.get()](auto const &, ux::Input::PointerRoutedEventArgs const &args) {
      POINT point{};
      if (state->events.headerDrag && args.GetCurrentPoint(state->titleBar).Properties().IsLeftButtonPressed() &&
          GetCursorPos(&point) &&
          SendMessageW(state->parent, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y)) == HTCAPTION) {
        args.Handled(true);
        state->events.headerDrag(point);
      }
    });
    state->island.Content(state->root);
    state->islandWindow = ::winrt::Microsoft::UI::GetWindowFromWindowId(state->island.SiteBridge().WindowId());
    if (!state->islandWindow || !SetWindowSubclass(state->islandWindow, State::islandInput, 1,
                                                   reinterpret_cast<DWORD_PTR>(state.get()))) {
      state.reset();
      return;
    }
    state->makeHeader();
    state->showActions(false);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(state->factory.put()));
    state->makeView(false, 32);
    state->configureSelection();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"create surface", error);
    state.reset();
  }
}

Surface::~Surface() = default;
bool Surface::available() const { return state && state->islandWindow && state->view; }

void Surface::place(RECT bounds) {
  if (!available()) return;
  try {
    const auto width = std::max(1L, bounds.right - bounds.left);
    const auto height = std::max(1L, bounds.bottom - bounds.top);
    state->island.SiteBridge().MoveAndResize({bounds.left, bounds.top, width, height});
    state->positionHeader();
    state->positionOutline();
    state->updatePageLayout();
    state->positionPageDots();
  } catch (...) {}
}

void Surface::show(bool visible) {
  if (!available()) return;
  try {
    const auto bridge = state->island.SiteBridge();
    if (visible) bridge.Show();
    else bridge.Hide();
  } catch (...) {}
}

void Surface::animateReflow(bool resizing) {
  if (!available()) return;
  try { state->setReflow(resizing); } catch (...) {}
}

void Surface::animateZoom(bool zoomIn) {
  if (!available()) return;
  BOOL enabled = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) && !enabled) return;
  try {
    if (state->zoomAnimation) state->zoomAnimation.Stop();
    ux::Media::ScaleTransform transform;
    state->view.RenderTransformOrigin({0.5f, 0.5f});
    state->view.RenderTransform(transform);
    ux::Media::Animation::Storyboard storyboard;
    ux::Media::Animation::CubicEase easing;
    easing.EasingMode(ux::Media::Animation::EasingMode::EaseOut);
    for (const auto property : {L"(UIElement.RenderTransform).(ScaleTransform.ScaleX)",
                                L"(UIElement.RenderTransform).(ScaleTransform.ScaleY)"}) {
      ux::Media::Animation::DoubleAnimation animation;
      animation.From(::winrt::box_value(zoomIn ? 0.82 : 1.15).as<::winrt::Windows::Foundation::IReference<double>>());
      animation.To(::winrt::box_value(1.0).as<::winrt::Windows::Foundation::IReference<double>>());
      animation.Duration({std::chrono::milliseconds(150), ux::DurationType::TimeSpan});
      animation.FillBehavior(ux::Media::Animation::FillBehavior::Stop);
      animation.EasingFunction(easing);
      ux::Media::Animation::Storyboard::SetTarget(animation, state->view);
      ux::Media::Animation::Storyboard::SetTargetProperty(animation, property);
      storyboard.Children().Append(animation);
    }
    state->zoomAnimation = storyboard;
    storyboard.Begin();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"animate icon zoom", error);
  }
}

bool Surface::onFrame(std::function<void()> callback) {
  if (!available()) return false;
  try {
    state->frameCallback = std::move(callback);
    if (state->frameCallback && !state->frameToken) {
      state->frameToken = ux::Media::CompositionTarget::Rendering(
          [state = state.get()](const IInspectable &, const IInspectable &) {
            auto frame = state->frameCallback;
            if (frame) frame();
          });
    } else if (!state->frameCallback && state->frameToken) {
      ux::Media::CompositionTarget::Rendering(state->frameToken);
      state->frameToken = {};
    }
    return true;
  } catch (...) {
    state->frameCallback = {};
    return false;
  }
}

void Surface::rows(const std::vector<Item> &items, bool listMode, int logicalIconSize, int spacing) {
  if (!available()) return;
  state->synchronizing = true;
  try {
    state->endSelection(true);
    state->spacing = std::clamp(spacing, 0, 24);
    state->makeView(listMode, logicalIconSize);
    state->items.clear();
    state->items.reserve(items.size());
    std::vector<IInspectable> indices;
    indices.reserve(items.size());
    for (const auto &item : items) {
      ux::Media::ImageSource image{nullptr};
      try { image = imageSource(state->factory.get(), item.image); } catch (...) {}
      indices.push_back(::winrt::box_value(static_cast<int>(state->items.size())));
      state->items.push_back({item.name, image});
    }
    state->view.ItemsSource(::winrt::single_threaded_observable_vector<IInspectable>(std::move(indices)));
    state->root.InvalidateMeasure();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"update surface", error);
  }
  state->synchronizing = false;
}

void Surface::itemSpacing(int spacing) {
  if (!available()) return;
  try {
    const auto value = std::clamp(spacing, 0, 24);
    if (state->spacing == value) return;
    state->spacing = value;
    state->updatePageLayout();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"update item spacing", error);
  }
}

int Surface::pageCapacity(bool listMode, int logicalIconSize, int titleHeight, int cornerRadius, int spacing) const {
  if (!available()) return 1;
  const auto metrics = measurePage(state->parent, listMode, logicalIconSize, titleHeight, cornerRadius, std::clamp(spacing, 0, 24));
  return metrics.columns * metrics.rows;
}

void Surface::pagination(int page, int pages, std::function<void(int)> navigate) {
  if (!available()) return;
  try {
    const bool changed = (state->pageCount > 0) != (pages > 0);
    if (pages <= 0) {
      state->pageDots.Children().Clear();
      state->dots.clear();
      state->firstDot = -1;
    }
    state->pageCount = pages;
    state->page = std::clamp(page, 0, std::max(0, pages - 1));
    state->navigatePage = std::move(navigate);
    if (changed) {
      state->positionBody();
      state->configurePageScroll();
      state->updatePageLayout();
    }
    state->positionPageDots();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"update display pagination", error);
  }
}

void Surface::appearance(bool dark, bool highContrast, COLORREF tint, BYTE opacity, bool glass) {
  if (!available()) return;
  try {
    state->highContrast = highContrast;
    state->updateRealizedItems();
    state->positionPageDots();
    state->root.RequestedTheme(highContrast ? ux::ElementTheme::Default
                                           : dark ? ux::ElementTheme::Dark : ux::ElementTheme::Light);
    // Below Acrylic's tint floor, blend the chosen color directly over the desktop.
    constexpr int minimumAcrylicOpacity = 64;
    if (opacity >= minimumAcrylicOpacity && glass && !highContrast && backdrops::DesktopAcrylicController::IsSupported()) {
      if (!state->acrylic) state->acrylic = ::winrt::make_self<BoxAcrylic>();
      state->acrylic->appearance(dark, tint);
      if (!state->acrylicAttached) {
        state->island.SystemBackdrop(state->acrylic.as<ux::Media::SystemBackdrop>());
        state->acrylicAttached = true;
      }
      if (state->acrylic->controller) {
        const auto overlay = static_cast<BYTE>(MulDiv(opacity - minimumAcrylicOpacity, 255, 255 - minimumAcrylicOpacity));
        state->backgroundTint.Color(windowsColor(tint, overlay));
        return;
      }
    }
    if (state->acrylicAttached) {
      state->island.SystemBackdrop(nullptr);
      state->acrylicAttached = false;
    }
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"configure native acrylic", error);
  }
  try { state->backgroundTint.Color(windowsColor(tint, highContrast ? 255 : opacity)); } catch (...) {}
}

bool Surface::acrylicActive() const {
  if (!available() || !state->acrylicAttached || !state->acrylic || !state->acrylic->controller) return false;
  try { return state->acrylic->controller.State() == backdrops::SystemBackdropState::Active; } catch (...) { return false; }
}

void Surface::header(const std::vector<std::wstring> &titles, std::size_t active, bool collapsed,
                     bool actionsVisible, bool highlighted, int radius, COLORREF text, COLORREF muted,
                     COLORREF border, COLORREF accent, int height, TitleStyle style, HICON icon) {
  if (!available()) return;
  try {
    if (collapsed) state->endSelection(true);
    const bool changedTitles = state->titles != titles || state->titleStyle != style;
    state->titleStyle = style;
    state->headerHeight = height;
    state->titleBar.Height(height);
    state->activeTab = active;
    state->collapsed = collapsed;
    if (collapsed) state->updateResizeCursor(HTCLIENT);
    state->highlighted = highlighted;
    state->cornerRadius = radius;
    state->textColor = text;
    state->paintSelectedItems();
    state->mutedColor = muted;
    state->borderColor = border;
    state->accentColor = accent;
    state->body.Visibility(collapsed ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    state->positionBody();
    state->updatePageLayout();
    state->positionPageDots();
    state->resizeGrip.Visibility(collapsed ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    for (const auto &line : state->resizeLines) line.Stroke(solidBrush(muted, state->highContrast ? 255 : 160));
    if (changedTitles) {
      state->titles = titles;
      state->makeTabs();
      state->title.Text(titles.empty() ? L"" : titles.front());
    }
    if (state->cachedIcon != icon) {
      state->cachedIcon = icon;
      state->titleImage = imageSource(state->factory.get(), icon);
    }
    state->titleContent.Visibility(titles.size() > 1 ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    state->title.Visibility(style == TitleStyle::Icon ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    state->titleIcon.Visibility(style == TitleStyle::Text ? ux::Visibility::Collapsed : ux::Visibility::Visible);
    state->titleIcon.Source(state->titleImage);
    if (state->firstTabIcon) state->firstTabIcon.Source(state->titleImage);
    state->title.Foreground(solidBrush(text));
    state->showActions(actionsVisible);
    for (auto &button : state->buttons) state->paintHeaderButton(button, muted);
    auto &toggle = state->buttons[2];
    toggle.label.Text(collapsed ? L"\uE70D" : L"\uE70E");
    ux::Automation::AutomationProperties::SetName(toggle.box, lang::text(collapsed ? L"展开盒子" : L"折叠盒子"));
    state->paintTabs();
    state->paintOutline();
    state->positionHeader();
  } catch (const ::winrt::hresult_error &error) {
    logFailure(L"update box header", error);
  }
}

void Surface::showActions(bool visible) {
  if (!available()) return;
  try { state->showActions(visible); } catch (...) {}
}

void Surface::highlight(bool highlighted) {
  if (!available() || state->highlighted == highlighted) return;
  try {
    state->highlighted = highlighted;
    state->paintOutline();
  } catch (...) {}
}

bool Surface::headerInteractive(POINT screen) const {
  return available() && state->headerInteractive(screen);
}
std::optional<std::size_t> Surface::tabAt(POINT screen) const {
  if (!available() || !ScreenToClient(state->islandWindow, &screen)) return std::nullopt;
  const auto dpi = GetDpiForWindow(state->islandWindow);
  if (!dpi) return std::nullopt;
  const double x = screen.x * 96.0 / dpi, y = screen.y * 96.0 / dpi;
  if (y < State::headerPadding || y >= state->headerHeight) return std::nullopt;
  for (std::size_t i = 0; i < state->tabs.size(); ++i) {
    const auto &tab = state->tabs[i];
    if (x >= tab.left && x < tab.left + tab.width) return i;
  }
  return std::nullopt;
}
int Surface::hit(POINT point) const {
  if (!available()) return -1;
  try { return state->hit(point); } catch (...) { return -1; }
}

bool Surface::itemBounds(int row, RECT &screen) const {
  if (!available()) return false;
  try {
    const auto container = state->view.ContainerFromIndex(row).try_as<ux::FrameworkElement>();
    if (!container) return false;
    const auto bounds = container.TransformToVisual(state->root).TransformBounds(
        {0, 0, static_cast<float>(container.ActualWidth()), static_cast<float>(container.ActualHeight())});
    const double scale = GetDpiForWindow(state->islandWindow) / 96.0;
    POINT from{static_cast<LONG>(std::lround(bounds.X * scale)), static_cast<LONG>(std::lround(bounds.Y * scale))};
    ClientToScreen(state->islandWindow, &from);
    screen = {from.x, from.y, from.x + static_cast<LONG>(std::lround(bounds.Width * scale)),
              from.y + static_cast<LONG>(std::lround(bounds.Height * scale))};
    return !IsRectEmpty(&screen);
  } catch (...) { return false; }
}

bool Surface::scrollAt(POINT screen) {
  if (!available() || state->collapsed || state->pageCount > 0) return false;
  try {
    const auto dpi = GetDpiForWindow(state->islandWindow);
    RECT bounds{};
    if (!GetWindowRect(state->islandWindow, &bounds)) return false;
    bounds.top += MulDiv(static_cast<int>(state->headerHeight + contentTopPadding), dpi, 96);
    bounds.bottom -= MulDiv(std::max(10, state->cornerRadius), dpi, 96);
    if (!PtInRect(&bounds, screen)) return false;
    const auto scroll = visualChild<controls::ScrollViewer>(state->view);
    if (!scroll) return false;
    const auto edge = MulDiv(24, dpi, 96);
    const double delta = screen.y < bounds.top + edge ? -24.0 : screen.y >= bounds.bottom - edge ? 24.0 : 0.0;
    const double from = scroll.VerticalOffset();
    const double to = std::clamp(from + delta, 0.0, scroll.ScrollableHeight());
    return from != to && scroll.ChangeView(nullptr, ::winrt::box_value(to).as<::winrt::Windows::Foundation::IReference<double>>(),
                                          nullptr, true);
  } catch (...) { return false; }
}

void Surface::refreshImage(int row, HICON image) {
  if (!available() || row < 0 || static_cast<size_t>(row) >= state->items.size()) return;
  try {
    if (auto source = imageSource(state->factory.get(), image)) {
      state->items[row].image = source;
      state->apply(state->view.ContainerFromIndex(row).try_as<controls::ContentControl>(), row);
    }
  } catch (...) {}
}

void Surface::select(const std::vector<int> &rows) {
  if (!available()) return;
  state->synchronizing = true;
  try {
    auto selected = state->view.SelectedItems();
    selected.Clear();
    for (const auto row : rows) {
      if (row >= 0 && static_cast<size_t>(row) < state->items.size())
        selected.Append(state->view.Items().GetAt(row));
    }
  } catch (...) {}
  state->synchronizing = false;
}

}
