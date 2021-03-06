@echo off

SET CMD=%0
SET SRC=%~1
SET DST=%~2

SET DIR=%~dp0
SET SED_CMD=%DIR%\sed.exe
SET SED_SCRIPT=%ALLUSERSPROFILE%\Bareos\configure.sed

if not exist "%SED_CMD%" (
    echo Failed to find the command sed. It is expected in the same directory as %CMD% [%SED%].
    call :exit 1
)

echo source %SRC%
echo dest: %DST%

if "%SRC%" == "" (
    call :usage
)

if not exist "%SRC%\*" (
    echo Directory "%SRC%" does not exists.
    call :exit 1
)

if not exist "%SED_SCRIPT%" (
    echo Configure configuration [%SED_SCRIPT%] does not exists.
    call :exit 1
)

if "%DST%" == "" (
    call :usage
)


rem prepare destination directory
mkdir "%DST%"

if not exist "%DST%\*" (
    echo Directory "%DST%" does not exists.
    call :exit 1
)

pushd "%SRC%"
rem c: COMPONENT
for /D %%c in (*) do (
    popd
    echo component: %%c
    if exist "%DST%\%%c" (
        echo.  skipped, path %DST%\%%c already exists
    ) else (
        rem copy config templates to destination
        rem xcopy options:
        rem /E           Copies directories and subdirectories, including empty ones.
        rem /I           If destination does not exist and copying more than one file,
        rem                assumes that destination must be a directory.
        rem /O           Copies file ownership and ACL information.
        rem /Y           Suppresses prompting to confirm you want to overwrite
        rem                and existing destination file.
        xcopy /e /i /o /y "%SRC%\%%c" "%DST%\%%c"
        rem r: RESOURCE TYPE
        for /D %%r in ("%DST%\%%c"\*) do (
            echo.  resource: %%r
            rem f: file
            for %%f in ("%%r"\*.conf) do (
                echo.    %%f
                rem delete old backup
                if exist "%%f.orig" del "%%f.orig"
                "%SED_CMD%" --in-place=".orig" --file "%SED_SCRIPT%" "%%f"
            )
        )
    )
    pushd "%SRC%"
)
popd
rem call :exit
goto:eof

rem functions

:usage
    echo %CMD% source dest
    call :exit 1

:exit
    rem pause
    rem exit %*
    rem emulate Ctrl-C
    cmd /c exit -1073741510
