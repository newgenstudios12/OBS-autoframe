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