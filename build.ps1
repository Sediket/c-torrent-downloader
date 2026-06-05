$cmake = "C:\msys64\mingw64\bin\cmake.exe"
$root  = $PSScriptRoot

& $cmake --build "$root\build" --config Release
