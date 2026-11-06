// Exercise the production popup controller using hidden, test-owned windows.
// The hidden tests do not install the popup's short-lived input observers,
// change the real foreground window, or write user settings.
#include "../src/quick_popup.cpp"
#include <objbase.h>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace {
int checks = 0;
void check(bool value) { if (!value) throw std::runtime_error("popup assertion " + std::to_string(checks)); ++checks; }
std::shared_ptr<quick::popup::Branch> model() {
  using namespace quick::popup;
  auto children = std::make_shared<Branch>();
  children->items = {{L"子目录", 42}, {L"固定此文件夹", 43, Symbol::Star}};
  auto root = std::make_shared<Branch>();
  root->items = {{L"置顶", 0, Symbol::Star, {}, Row::Heading},
                 {L"项目 & 资料😀", 41, Symbol::Folder, children},
                 {L"历史记录", 0, Symbol::History, children}};
  return root;
}
std::shared_ptr<quick::popup::Branch> compactModel() {
  using namespace quick::popup;
  auto root = model();
  root->items[0].actions.push_back({51, Symbol::Star, L"置顶当前文件夹"});
  root->items[2].symbol = Symbol::More;
  Item toolbar; toolbar.row = Row::Toolbar;
  toolbar.actions = {{52, Symbol::Settings, L"设置"}, {53, Symbol::Trash, L"清空历史"}};
  root->items.push_back(std::move(toolbar)); return root;
}
}
int main() {
  using namespace quick::popup;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    auto instance = GetModuleHandleW(nullptr); registerPanel(instance);
    {
      Session session(instance, ui::ThemeMode::Dark); session.create(model(), {50, 50}, false);
      check(!session.done && session.panels.size() == 1);
      session.select(*session.panels[0], 1); session.expand(0, 1);
      check(!session.done && session.selected == 0 && session.panels.size() == 2); // Hover never navigates.
      session.activate(0, 1);
      check(session.done && session.selected == 41); // Parent row click opens that folder, not its child.
      session.activate(1, 0); check(session.selected == 41); // Queued click cannot navigate after closing.
    }
    {
      Session session(instance, ui::ThemeMode::Light); session.create(model(), {50, 50}, false);
      session.key(VK_DOWN); check(session.panels[0]->selected == 1); // Skip section headings.
      session.key(VK_RIGHT); check(session.panels.size() == 2 && session.active == 1 && session.panels[1]->selected == 0);
      session.key(VK_LEFT); check(session.panels.size() == 1 && session.active == 0);
      session.key(VK_RETURN); check(session.done && session.selected == 41);
    }
    {
      Session session(instance, ui::ThemeMode::Dark); auto &panel = session.create(model(), {50, 50}, false);
      RECT item{}; SendMessageW(panel.list, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&item));
      POINT point{item.left + 30, (item.top + item.bottom) / 2}; ClientToScreen(panel.list, &point);
      session.mouse(point); SendMessageW(session.root(), WM_TIMER, hoverTimer, 0);
      check(session.panels.size() == 2 && !session.done);
      ScreenToClient(session.root(), &point);
      SendMessageW(session.root(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
      SendMessageW(session.root(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
      check(session.done && session.selected == 41); // Actual list hit-testing and mouse dispatch.
    }
    {
      Session session(instance, ui::ThemeMode::Dark); session.create(model(), {50, 50}, false);
      session.activate(0, 2); check(!session.done && session.panels.size() == 2); // History click expands.
      session.key(VK_RETURN); check(session.done && session.selected == 42);
    }
    for (UINT event : {UINT(WM_ACTIVATEAPP), UINT(WM_ACTIVATE), UINT(WM_CAPTURECHANGED), UINT(WM_CANCELMODE), UINT(WM_CLOSE)}) {
      Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
      session.expand(0, 1); session.armed = true;
      SendMessageW(session.root(), event, 0, 0);
      check(session.done && !session.armed && session.selected == 0);
      for (const auto &panel : session.panels) check(!IsWindowVisible(panel->window));
    }
    {
      Session session(instance, ui::ThemeMode::Dark); session.create(model(), {50, 50}, false);
      session.expand(0, 1); session.armed = true;
      check(session.family(session.panels[1]->list));
      SendMessageW(session.root(), WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(session.panels[1]->window));
      check(!session.done); // Moving within the popup family must not dismiss it.
      SendMessageW(session.root(), WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(session.panels[1]->window));
      check(!session.done);
      session.key(VK_ESCAPE); check(session.done && session.selected == 0);
    }
    {
      Session session(instance, ui::ThemeMode::System); auto root = model(); int polls = 0;
      root->items[1].children->poll = [&](Branch &) { ++polls; return false; };
      session.create(root, {50, 50}, false); session.expand(0, 1); check(polls == 1);
      session.finish(); SendMessageW(session.root(), WM_TIMER, loadingTimer, 0);
      check(polls == 1); // A completed dismissal cannot restart asynchronous loading.
    }
    {
      // Regression: the background helper cannot assume foreground permission.
      // A popup must remain open while its original Explorer/dialog is active.
      HWND anchor = CreateWindowExW(0, L"STATIC", L"test anchor", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
      HWND other = CreateWindowExW(0, L"STATIC", L"test other", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
      check(anchor && other);
      {
        Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
        session.anchor = anchor; session.armed = true;
        session.observeForeground(anchor); check(!session.done && !session.activated);
        session.expand(0, 1); check(!session.done && session.panels.size() == 2);
        session.key(VK_DOWN); session.key(VK_RETURN);
        check(session.done && session.selected == 41); // Keyboard can navigate without a focus handoff.
      }
      {
        Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
        session.anchor = anchor; session.armed = true;
        session.observeForeground(anchor); session.observeForeground(other);
        check(session.done && !session.selected); // Switching apps closes an unactivated menu too.
      }
      {
        Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
        session.anchor = anchor; session.armed = true;
        session.observeForeground(session.root()); check(!session.done && session.activated);
        session.observeForeground(anchor); check(session.done); // After a real activation, returning to Explorer is focus loss.
      }
      {
        Session session(instance, ui::ThemeMode::System); auto &panel = session.create(model(), {50, 50}, false);
        session.anchor = anchor; session.armed = true;
        RECT row{}; SendMessageW(panel.list, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&row));
        auto point = MAKELPARAM(row.left + 30, (row.top + row.bottom) / 2);
        SendMessageW(panel.list, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(panel.list, WM_LBUTTONUP, 0, point);
        check(session.done && session.selected == 41); // Native list events work without global mouse capture.
      }
      {
        Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
        SendMessageW(session.root(), WM_DPICHANGED, MAKELONG(144, 144), 0);
        check(!session.done); // Initial monitor placement is not dismissal.
        session.anchor = anchor; session.armed = true;
        SendMessageW(session.root(), dismiss, 0, 0); check(session.done);
      }
      DestroyWindow(other); DestroyWindow(anchor);
    }
    for (int repetition = 0; repetition < 6; ++repetition) {
      Session session(instance, ui::ThemeMode::System); session.create(model(), {50, 50}, false);
      session.armed = true;
      HWND window = session.root();
      // Sent messages and out-of-context WinEvent callbacks run INSIDE
      // GetMessage. Changing done alone leaves it waiting indefinitely.
      check(SetTimer(window, 99, 1000, nullptr) != 0);
      std::thread sender([window] {
        Sleep(10); DWORD_PTR result = 0;
        SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1500, &result);
      });
      MSG message{}; bool woke = false;
      while (GetMessageW(&message, window, 0, 0) > 0) {
        if (message.message == WM_NULL && session.done) { woke = true; break; }
        if (message.message == WM_TIMER && message.wParam == 99) break;
        // Asynchronous icon completion is unrelated to closing. Pump it just
        // like the real popup loop, still requiring finish's explicit wakeup.
        TranslateMessage(&message); DispatchMessageW(&message);
      }
      sender.join(); KillTimer(window, 99);
      check(woke && session.done);
    }
    for (UINT dpi : {96u, 144u, 192u}) {
      Session session(instance, ui::ThemeMode::System); auto &panel = session.create(compactModel(), {50, 50}, false);
      session.dpi = dpi; session.populate(panel); session.place(panel, {50, 50}, false);
      auto pointFor = [&](int row, int action) {
        RECT bounds{}; SendMessageW(panel.list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&bounds));
        if (action >= 0) bounds = session.actionRect(panel.branch->items[row], bounds, action);
        return POINT{action < 0 ? bounds.left + 15 : (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
      };
      auto click = [&](POINT point) {
        SendMessageW(panel.list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
        SendMessageW(panel.list, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
      };
      click(pointFor(0, -1)); check(!session.done); // Heading text/blank space cannot pin a folder.
      click(pointFor(3, -1)); check(!session.done); // Footer blank space has no destructive action.
      auto settings = pointFor(3, 0), clear = pointFor(3, 1);
      SendMessageW(panel.list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(settings.x, settings.y));
      SendMessageW(panel.list, WM_LBUTTONUP, 0, MAKELPARAM(clear.x, clear.y));
      check(!session.done); // Pressing settings then releasing over clear must not execute either.
      auto star = pointFor(0, 0), screen = star; ClientToScreen(panel.list, &screen);
      session.mouse(screen); check(session.tipText == L"置顶当前文件夹");
      session.key(VK_RIGHT); check(session.tipText.empty()); // Keyboard movement cancels stale tooltip.
      click(star); check(session.done && session.selected == 51);
    }
    for (int action = 0; action < 2; ++action) {
      Session session(instance, ui::ThemeMode::Dark); auto &panel = session.create(compactModel(), {50, 50}, false);
      RECT row{}; SendMessageW(panel.list, LB_GETITEMRECT, 3, reinterpret_cast<LPARAM>(&row));
      auto bounds = session.actionRect(panel.branch->items[3], row, action);
      auto point = MAKELPARAM((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
      SendMessageW(panel.list, WM_LBUTTONDOWN, MK_LBUTTON, point); SendMessageW(panel.list, WM_LBUTTONUP, 0, point);
      check(session.done && session.selected == unsigned(52 + action));
    }
    {
      Session session(instance, ui::ThemeMode::Light); auto &panel = session.create(compactModel(), {50, 50}, false);
      session.key(VK_HOME); check(panel.selected == 0 && panel.selectedAction == 0);
      session.key(VK_END); check(panel.selected == 3 && panel.selectedAction == 0);
      session.key(VK_RIGHT); check(panel.selectedAction == 1);
      session.key(VK_LEFT); check(panel.selectedAction == 0);
      session.key(VK_RETURN); check(session.done && session.selected == 52);
    }
    {
      Session session(instance, ui::ThemeMode::Dark); session.create(compactModel(), {50, 50}, false);
      session.select(*session.panels[0], 2); session.key(VK_RIGHT);
      check(!session.done && session.active == 1); // Ellipsis exposes history like the old labelled row.
      session.key(VK_RETURN); check(session.done && session.selected == 42);
    }
    std::cout << "PASS " << checks << " popup checks: direct click, hover, nested focus, dismissal, keyboard, canceled loading\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; CoUninitialize(); return 1; }
  CoUninitialize(); return 0;
}
