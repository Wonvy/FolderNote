#include "../src/update.h"
#include "../src/core.h"
#include <iostream>
#include <stdexcept>

static void check(bool value) { if (!value) throw std::runtime_error("release selection assertion"); }
static std::string asset(const std::string &name, std::string digest = "sha256:" + std::string(64, 'a'),
                         std::string repository = "Wonvy/ShibaKit") {
  return "{\"name\":\"" + name + "\",\"digest\":\"" + digest +
      "\",\"browser_download_url\":\"https://github.com/" + repository + "/releases/download/v1.9.7/" + name + "\"}";
}
static auto parse(const std::string &assets, bool modern = false, const wchar_t *current = L"1.9.6") {
  return updater::parseRelease("{\"tag_name\":\"v1.9.7\",\"assets\":[" + assets + "]}", L"Wonvy/ShibaKit", current, modern);
}
template <typename Action> static void rejects(Action action) {
  bool failed = false;
  try { action(); } catch (const fn::Error &) { failed = true; }
  check(failed);
}
int main() {
  try {
    const auto zip = asset("ShibaKit-win-x64.zip");
    const auto setup = asset("ShibaKit-Setup-x64.exe");
    auto result = parse(zip + "," + setup);
    check(result && result->standaloneInstaller && !result->win11Package && result->version == L"1.9.7");
    check(parse(setup + "," + zip)->standaloneInstaller);
    check(!parse(zip)->standaloneInstaller);
    const auto modernZip = asset("ShibaKit-win11-test.zip");
    const auto modernSetup = asset("ShibaKit-Setup-Win11-x64.exe");
    result = parse(setup + "," + modernZip + "," + modernSetup, true);
    check(result && result->standaloneInstaller && result->win11Package);
    check(!parse(modernZip, true)->standaloneInstaller);
    check(!parse(setup, false, L"1.9.7"));
    check(!parse(setup, false, L"2.0.0"));
    rejects([&] { parse(modernSetup); });
    rejects([&] { parse(setup, true); });
    rejects([&] { parse(asset("ShibaKit-Setup-x64.exe", "sha256:bad") + "," + zip); });
    rejects([&] { parse(asset("ShibaKit-Setup-x64.exe", "sha256:" + std::string(64, 'g'))); });
    rejects([&] { parse(asset("ShibaKit-Setup-x64.exe", "sha256:" + std::string(64, 'a'), "Other/Repository")); });
    rejects([] { updater::parseRelease("invalid", L"Wonvy/ShibaKit", L"1.9.6"); });
    const auto withNotes = updater::parseRelease(
        "{\"tag_name\":\"v1.9.7\",\"body\":\"下载 **ShibaKit-Setup-x64.exe** 即可安装，无需解压。\\n\\nZIP 用于兼容旧版在线更新。\\n\\n### 修复与优化\\r\\n- **修复拖拽弹窗**：解决拖出文件报错\\r\\n- 优化省电定时器\",\"assets\":[" + setup + "]}",
        L"Wonvy/ShibaKit", L"1.9.6", false);
    check(withNotes && withNotes->notes == L"修复与优化\r\n• 修复拖拽弹窗：解决拖出文件报错\r\n• 优化省电定时器");
    std::cout << "Release selection, backward compatibility, and integrity checks passed.\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    catch (const fn::Error &) { std::cerr << "Unexpected release error\n"; }
  return 1;
}
