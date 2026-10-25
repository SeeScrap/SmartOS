# NOTE: our HAL lives in src/board/, not src/hal/, on purpose.
# ESP-IDF ships components/hal/platform_port/include/hal/log.h. A quoted
# include from inside src/hal/ searches its own directory first, misses, then
# falls through to the include path and picks up IDF's hal/log.h instead of
# ours - so "hal/" is a namespace we cannot use. Do not rename this back.

# Apps are picked up by directory, not listed: adding one is dropping a .cpp
# into src/apps/ (stage 4.3c).
#
# A glob is only evaluated at configure time, so a NEW app file needs one
# re-configure: delete the build folder .pio/build/s3-n16r8/ and build, the same
# as after a build_flags change. CONFIGURE_DEPENDS would automate that, but
# ESP-IDF also reads this file in CMake script mode to work out requirements,
# and CONFIGURE_DEPENDS is an error there.
file(GLOB SMARTOS_APP_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/apps/*.cpp")

idf_component_register(
    SRCS
        "main.cpp"
        "boot_gfx.cpp"
        "system.cpp"
        "tests/harness.cpp"
        "tests/test_loader.cpp"
        "tests/test_vm.cpp"
        "tests/test_appload.cpp"
        "tests/test_kernel.cpp"
        "tests/test_kernel_stress.cpp"
        "tests/test_park.cpp"
        "tests/test_input.cpp"
        "tests/test_evbus.cpp"
        "tests/test_gfx.cpp"
        "tests/test_sdk.cpp"
        "tests/test_ui.cpp"
        "tests/test_apps.cpp"
        "tests/test_fs.cpp"
        "tests/test_sys.cpp"
        "tests/test_limits.cpp"
        "tests/test_look.cpp"
        "tests/test_radio.cpp"
        "board/log.cpp"
        "board/lcd_bus.cpp"
        "board/lcd_panel.cpp"
        "board/lcd_probe.cpp"
        "board/input.cpp"
        "gfx/fb.cpp"
        "gfx/image.cpp"
        "gfx/wallpaper_data.cpp"
        "gfx/text.cpp"
        "os/evbus.cpp"
        "os/vm.cpp"
        "os/vm_sys.cpp"
        "os/appload.cpp"
        "os/krand.cpp"
        "os/app.cpp"
        "os/boot.cpp"
        "os/dev.cpp"
        "os/fs.cpp"
        "os/syslog.cpp"
        "os/settings.cpp"
        "os/screen.cpp"
        "os/radio.cpp"
        "os/shell.cpp"
        "os/sysinfo.cpp"
        "ui/ui.cpp"
        "ui/keyboard.cpp"
        ${SMARTOS_APP_SRCS}
    INCLUDE_DIRS "." "../include"
    REQUIRES
        smartos         # the kernel, the architecture and the port to this chip
        driver          # spi_master, gpio
        esp_adc         # the device service's ADC (stage 4.3c)
        esp_driver_gptimer  # the device service's sampling interrupt
        esp_hal_usb     # the shell reads the USB Serial/JTAG FIFO (stage 5.1)
        esp_hw_support  # os/sysinfo.cpp reads the chip id
        esp_partition   # the park test scribbles on the storage partition
        esp_psram
        esp_wifi        # stage 6.3: the radio
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        freertos
        soc
    # An app file is referenced by nothing - it registers itself from a static
    # constructor - and a linker taking objects from an ordinary archive only
    # pulls in the ones something needs. Defensive, as it turns out: stage
    # 4.3c built without this and app_hello.cpp, which nothing mentions, was
    # still linked and registered, so this build keeps the component's objects
    # whole regardless. It stays so that the one-file rule does not quietly
    # depend on that.
    WHOLE_ARCHIVE
)

# The --wrap flags that park core 1 around a flash write moved to
# components/smartos/CMakeLists.txt in R7a: they belong beside port/kpark.cpp,
# which implements the wrappers.
