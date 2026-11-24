function(foldernote_nuget_package result id version checksum)
  string(TOLOWER "${id}" package)
  set(package_cache "$ENV{NUGET_PACKAGES}")
  if(NOT package_cache)
    set(package_cache "$ENV{USERPROFILE}/.nuget/packages")
  endif()

  set(folder "${CMAKE_BINARY_DIR}/_deps/${package}-${version}")
  set(filename "${package}.${version}.nupkg")
  set(archive "${package_cache}/${package}/${version}/${filename}")
  if(NOT EXISTS "${archive}")
    set(archive "${CMAKE_BINARY_DIR}/_deps/${filename}")
  endif()
  if(NOT EXISTS "${archive}")
    file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/${package}/${version}/${filename}" "${archive}"
         EXPECTED_HASH "SHA256=${checksum}" TLS_VERIFY ON STATUS status)
    list(GET status 0 result_code)
    if(NOT result_code EQUAL 0)
      message(FATAL_ERROR "Unable to restore ${id} ${version}: ${status}")
    endif()
  endif()
  file(SHA256 "${archive}" digest)
  if(NOT digest STREQUAL checksum)
    message(FATAL_ERROR "Unexpected SHA256 for ${archive}")
  endif()
  if(NOT EXISTS "${folder}/${package}.nuspec")
    file(MAKE_DIRECTORY "${folder}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${folder}")
  endif()
  set(${result} "${folder}" PARENT_SCOPE)
endfunction()

function(foldernote_add_winui target application)
  if(NOT MSVC OR NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    message(FATAL_ERROR "The WinUI desktop requires a Visual Studio generator and Windows SDK.")
  endif()
  cmake_host_system_information(RESULT sdk_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW BOTH)
  set(sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
  set(metadata "${sdk_root}/UnionMetadata/${sdk_version}/Windows.winmd")
  find_program(cppwinrt_tool NAMES cppwinrt.exe
    HINTS "${sdk_root}/bin/${sdk_version}/x64" "${sdk_root}/bin/${sdk_version}/x86")
  if(NOT cppwinrt_tool OR NOT EXISTS "${metadata}")
    message(FATAL_ERROR "The Windows SDK C++/WinRT projection tool and Windows metadata are required.")
  endif()
  if(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(architecture arm64)
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(architecture x64)
  else()
    set(architecture x86)
  endif()

  # Match the versions distributed together in Windows App SDK 1.8.260921001.
  foldernote_nuget_package(winui Microsoft.WindowsAppSDK.WinUI 1.8.260803003
    bdf72c0cbc385ba6e6ab916b17d3aa00950811c9d6372d11ac2f32f1c83f2496)
  foldernote_nuget_package(foundation Microsoft.WindowsAppSDK.Foundation 1.8.260803002
    b9232041afd605b606c6f78f442d92ead0076453f1f2a3260d2b7f8089bcab0e)
  foldernote_nuget_package(interactive Microsoft.WindowsAppSDK.InteractiveExperiences 1.8.260708001
    496eea92d353b5d3601b67353f06dcadd6d2d9b635575acebe6e42587dbfad76)
  # WinUI metadata references WebView2 types. The fence does not use WebView2.
  foldernote_nuget_package(webview Microsoft.Web.WebView2 1.0.3179.45
    70bd381c5d67f97a6a80294001cde125ab5b24b6d36098ad901d8e15efd752d0)

  file(GLOB projections_in CONFIGURE_DEPENDS
    "${winui}/metadata/*.winmd"
    "${interactive}/metadata/10.0.18362.0/*.winmd")
  list(APPEND projections_in "${foundation}/metadata/Microsoft.Windows.ApplicationModel.Resources.winmd"
                             "${webview}/lib/Microsoft.Web.WebView2.Core.winmd")
  set(generated "${CMAKE_BINARY_DIR}/generated/winui/${sdk_version}")
  set(projection "${generated}/winrt/Microsoft.UI.Xaml.Hosting.h")
  add_custom_command(OUTPUT "${projection}"
    COMMAND "${cppwinrt_tool}" -input ${projections_in} -reference "${metadata}" -output "${generated}"
    DEPENDS ${projections_in} "${metadata}" VERBATIM)
  add_custom_target(foldernote_winui_projection DEPENDS "${projection}")
  add_dependencies(${target} foldernote_winui_projection)
  target_sources(${target} PRIVATE src/desktop_groups_winui.cpp)
  target_include_directories(${target} PRIVATE "${generated}" "${foundation}/include" "${interactive}/include"
                                            "${winui}/include" "${sdk_root}/Include/${sdk_version}/cppwinrt")
  target_compile_definitions(${target} PUBLIC FOLDERNOTE_WINUI=1)
  target_link_libraries(${target} PRIVATE oleaut32 windowscodecs)

  set(bootstrap "${foundation}/runtimes/win-${architecture}/native/Microsoft.WindowsAppRuntime.Bootstrap.dll")
  add_custom_command(TARGET ${application} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${bootstrap}" "$<TARGET_FILE_DIR:${application}>"
    VERBATIM)
  install(FILES "${bootstrap}" DESTINATION .)
endfunction()
