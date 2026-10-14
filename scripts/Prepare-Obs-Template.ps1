# Assemble our native AutoFrame plugin inside OBS's supported Windows plugin template.
# Used only on a GitHub-hosted Windows runner.
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path "$PSScriptRoot/..").Path
$template = Join-Path $project 'obs-template'
if (-not (Test-Path "$template/CMakeLists.txt")) {
    throw 'The OBS plugin template has not been checked out.'
}
$manifestPath = Join-Path $template 'buildspec.json'
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$manifest.name = 'obs-autoframe'
$manifest.displayName = 'OBS AutoFrame'
$manifest.version = '0.2.0'
$manifest.author = 'OBS AutoFrame contributors'
$manifest.website = 'https://github.com/obsproject/obs-plugintemplate'
$manifest | ConvertTo-Json -Depth 30 | Set-Content -Path $manifestPath -Encoding utf8
Copy-Item -Path "$project/src/*.cpp" -Destination "$template/src" -Force
Copy-Item -Path "$project/src/*.hpp" -Destination "$template/src" -Force
$cmake = Get-Content (Join-Path $template 'CMakeLists.txt') -Raw
if (-not $cmake.Contains('src/plugin-main.c')) { throw 'Unexpected upstream OBS plugin template CMakeLists layout.' }
$cmake = $cmake.Replace('src/plugin-main.c', 'src/plugin.cpp')
# OBS's template bootstrap builds and installs libobs under obs-template/.deps.
# When vcpkg is the top-level toolchain, the SDK's prefix can be missed.
# Discover its *installed* config only after the template's include(defaults).
$libobsLookup = @'
# Explicit OBS CMake dependency discovery for the Windows template.
# include(defaults) fetches OBS deps, but vcpkg's find_package shim can
# miss these prefixes. Crucially, CMake package config files for both
# libobs and w32-pthreads live directly inside .deps/cmake, so add .deps too.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
message(STATUS "AutoFrame: added installed OBS SDK prefix: ${CMAKE_CURRENT_SOURCE_DIR}/.deps")
file(GLOB _af_prefix_candidates LIST_DIRECTORIES true
     "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-*-x64")
foreach(_af_prefix IN LISTS _af_prefix_candidates)
  if(IS_DIRECTORY "${_af_prefix}")
    list(PREPEND CMAKE_PREFIX_PATH "${_af_prefix}")
    message(STATUS "AutoFrame: added OBS dependency prefix: ${_af_prefix}")
  endif()
endforeach()
list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
# Prefer the actual exported win32-threading package over a guessed path.
file(GLOB_RECURSE _af_winthread_configs LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/.deps/*w32-pthreadsConfig.cmake"
     "${CMAKE_CURRENT_SOURCE_DIR}/.deps/*w32-pthreads-config.cmake")
if(_af_winthread_configs)
  list(GET _af_winthread_configs 0 _af_thread_config)
  get_filename_component(_af_thread_dir "${_af_thread_config}" DIRECTORY)
  set("w32-pthreads_DIR" "${_af_thread_dir}" CACHE PATH "OBS w32-pthreads package location" FORCE)
  message(STATUS "AutoFrame: found installed w32-pthreads SDK config: ${_af_thread_config}")
else()
  message(STATUS "AutoFrame: w32-pthreads config not found yet; possible incomplete OBS SDK installation")
endif()
# This runs after include(defaults), which provisions .deps.
file(GLOB_RECURSE _af_obs_configs LIST_DIRECTORIES false
     "${CMAKE_CURRENT_SOURCE_DIR}/.deps/*libobsConfig.cmake"
     "${CMAKE_CURRENT_SOURCE_DIR}/.deps/*libobs-config.cmake")
if(_af_obs_configs)
  list(GET _af_obs_configs 0 _af_obs_config)
  get_filename_component(_af_libobs_dir "${_af_obs_config}" DIRECTORY)
  set(libobs_DIR "${_af_libobs_dir}" CACHE PATH "AutoFrame OBS SDK config" FORCE)
  message(STATUS "AutoFrame: found libobs SDK config: ${_af_obs_config}")
else()
  message(WARNING "AutoFrame: no installed libobs config found inside .deps; inspect the SDK installation.")
endif()
find_package(libobs REQUIRED)
'@
if (-not $cmake.Contains('find_package(libobs REQUIRED)')) {
    throw 'Could not find the official plugin template libobs package lookup.'
}
$cmake = $cmake.Replace('find_package(libobs REQUIRED)', $libobsLookup.Trim())
$cmake += @'

# OBS AutoFrame native tracking dependencies.
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc objdetect dnn)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)
if(MSVC)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
'@
Set-Content -Path (Join-Path $template 'CMakeLists.txt') -Value $cmake -Encoding utf8
$locale = Join-Path $template 'data/locale/en-US.ini'
New-Item -ItemType Directory -Force -Path (Split-Path $locale) | Out-Null
Set-Content -Path $locale -Value 'PluginName="OBS AutoFrame"' -Encoding utf8
Write-Host 'Prepared OBS plugin template with AutoFrame sources.'