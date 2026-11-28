#include "../src/quick_menu_gate.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
int checks = 0;
void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
bool anotherThreadCanAcquire(const std::wstring &name, DWORD timeout = 20) {
  bool acquired = false;
  std::thread worker([&] { quick::detail::MenuGate gate(name.c_str()); acquired = gate.acquire(timeout); });
  worker.join(); return acquired;
}
struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value) CloseHandle(value); }
};
}

int main() {
  using quick::detail::MenuGate;
  using quick::detail::MenuRequest;
  try {
    const auto base = std::wstring(L"Local\\ShibaKit.MenuGateTests.") + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    const auto mutexName = base + L".Popup", windowA = base + L".WindowA", windowB = base + L".WindowB";
    MenuGate retainedHandle(mutexName.c_str());
    check(retainedHandle.acquire(0), "initial popup acquires gate");
    check(!anotherThreadCanAcquire(mutexName), "visible popup rejects another owner within bounded wait");
    retainedHandle.release();
    check(anotherThreadCanAcquire(mutexName), "released ownership permits next popup even while old handle exists");
    retainedHandle.release();
    check(anotherThreadCanAcquire(mutexName), "repeated release never corrupts ownership");

    try {
      MenuGate popup(mutexName.c_str());
      check(popup.acquire(0), "exception fixture acquires popup");
      throw 1;
    } catch (int) { }
    check(anotherThreadCanAcquire(mutexName), "exception unwinding releases popup before an error dialog");

    Handle abandoned;
    bool acquired = false;
    std::thread formerOwner([&] {
      abandoned.value = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
      acquired = abandoned.value && WaitForSingleObject(abandoned.value, 0) == WAIT_OBJECT_0;
      // Deliberately leave ownership at thread exit, without leaking the handle.
    });
    formerOwner.join();
    check(acquired && retainedHandle.acquire(0), "abandoned owner is recoverable");
    retainedHandle.release();

    MenuRequest oldCommand(windowA.c_str());
    check(oldCommand.valid() && !oldCommand.current(), "request mapping is valid but inactive before popup acquisition");
    {
      MenuGate popup(mutexName.c_str());
      check(popup.acquire(0), "first request owns visible popup");
      oldCommand.begin();
      check(oldCommand.current(), "accepted popup starts request generation");
      bool duplicateAccepted = false;
      std::thread rejected([&] {
        MenuRequest rejectedRequest(windowA.c_str());
        MenuGate nextPopup(mutexName.c_str());
        duplicateAccepted = nextPopup.acquire(20);
        if (duplicateAccepted) rejectedRequest.begin();
      });
      rejected.join();
      check(!duplicateAccepted && oldCommand.current(), "rejected duplicate does not invalidate visible popup command");
    }
    // show() has returned. The old command remains alive, simulating a delayed
    // directory check, while a different thread accepts the next middle click.
    bool newerAccepted = false, newerCurrent = false;
    std::thread nextPopup([&] {
      MenuGate gate(mutexName.c_str());
      MenuRequest newer(windowA.c_str());
      newerAccepted = gate.acquire(20) && newer.valid();
      if (newerAccepted) { newer.begin(); newerCurrent = newer.current(); }
    });
    nextPopup.join();
    check(newerAccepted && newerCurrent, "slow hidden command does not hold visible-popup gate");
    check(!oldCommand.current(), "same-target newer request supersedes old command after newer request handle closes");

    MenuRequest currentA(windowA.c_str()); currentA.begin();
    bool otherCurrent = false;
    std::thread otherWindow([&] { MenuRequest requestB(windowB.c_str()); requestB.begin(); otherCurrent = requestB.current(); });
    otherWindow.join();
    check(currentA.current() && otherCurrent, "different target windows retain independent commands");
    {
      MenuRequest rejected(windowA.c_str());
      check(rejected.valid() && !rejected.current() && currentA.current(), "opening mapping without accepting request does not advance generation");
    }
    check(currentA.current(), "closing another mapping handle does not reset live generation");

    Handle mapping; mapping.value = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, windowA.c_str());
    auto counter = static_cast<volatile LONG64 *>(MapViewOfFile(mapping.value, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LONG64)));
    check(counter != nullptr, "open isolated request counter for wrap regression");
    InterlockedExchange64(counter, -1); UnmapViewOfFile(const_cast<LONG64 *>(counter));
    currentA.begin();
    check(currentA.current(), "wrapped zero ticket is a valid active request");
    MenuRequest afterWrap(windowA.c_str()); afterWrap.begin();
    check(afterWrap.current() && !currentA.current(), "next wrapped request still invalidates previous ticket");
    std::cout << "PASS " << checks << " menu gate assertions: ownership, bounded contention, cleanup, request ordering and target isolation\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1;
  }
}
