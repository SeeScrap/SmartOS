# NOTE: our HAL lives in src/s3hal/, not src/hal/, on purpose.
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
        "port/kport_esp32s3.cpp"
        "boot_gfx.cpp"
        "system.cpp"
        "tests/harness.cpp"
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
        "s3hal/log.cpp"
        "s3hal/lcd_bus.cpp"
        "s3hal/lcd_panel.cpp"
        "s3hal/lcd_probe.cpp"
        "s3hal/input.cpp"
        "gfx/fb.cpp"
        "gfx/image.cpp"
        "gfx/wallpaper_data.cpp"
        "gfx/text.cpp"
        "os/evbus.cpp"
        "os/kfmt.cpp"
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
        "kernel/kcore1.cpp"
        "kernel/kfault.cpp"
        "kernel/kvectors.S"
        "kernel/ksched.cpp"
        "kernel/kswitch.S"
        "kernel/ktime.cpp"
        "kernel/kheap.cpp"
        "kernel/kmbox.cpp"
        "kernel/kpark.cpp"
        "kernel/ksync.cpp"
        ${SMARTOS_APP_SRCS}
    INCLUDE_DIRS "." "../include"
    REQUIRES
        driver          # spi_master, gpio
        esp_adc         # the device service's ADC (stage 4.3c)
        esp_driver_gptimer  # the device service's sampling interrupt
        esp_hal_usb     # the shell reads the USB Serial/JTAG FIFO (stage 5.1)
        esp_hw_support  # esp_cpu, core start/stall
        esp_partition   # the park test scribbles on the storage partition
        esp_psram
        esp_rom
        esp_wifi        # stage 6.3: the radio
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        freertos
        hal             # cpu_utility_ll
        soc
        xtensa          # SPILL_ALL_WINDOWS
    # An app file is referenced by nothing - it registers itself from a static
    # constructor - and a linker taking objects from an ordinary archive only
    # pulls in the ones something needs. Defensive, as it turns out: stage
    # 4.3c built without this and app_hello.cpp, which nothing mentions, was
    # still linked and registered, so this build keeps the component's objects
    # whole regardless. It stays so that the one-file rule does not quietly
    # depend on that.
    WHOLE_ARCHIVE
)

# Stage 4.4: every SPI flash operation parks core 1. ESP-IDF calls these two
# around each one; --wrap sends those calls to the wrappers in
# kernel/kpark.cpp, which park core 1 and then call the originals. See the
# note in kernel/kpark.h.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=spi_flash_disable_interrupts_caches_and_other_cpu"
    "-Wl,--wrap=spi_flash_enable_interrupts_caches_and_other_cpu"
    # Stage 4.4: a restart puts core 1 to sleep first - see kernel/kcore1.cpp.
    "-Wl,--wrap=esp_restart_noos")
