# ===========================================================================
# SmartOS: the kernel, the architecture it runs on, and the port to this chip.
# ===========================================================================
#
# Everything here is below the operating system. src/ holds the OS - the event
# bus, apps, the filesystem, the screen - and depends on this; nothing here
# depends on src/, and the REQUIRES list below is what holds that true.
#
# THE REQUIRES LIST IS THE CLAIM. Up to R7 the README said the kernel does not
# depend on ESP-IDF except through a named seam. Now CMake enforces it: if a
# kernel file reaches for a component that is not listed, the build stops. The
# list is deliberately short, and every entry says which file needs it.
#
# WHAT IS IN EACH DIRECTORY
#   kernel/  the scheduler, locks, heap, clock, mailbox, fault reporter,
#            printf. No ESP-IDF headers at all - checked by grep in CI's
#            place, by hand here, since R5b.
#   arch/    Xtensa, and only Xtensa: the vector table, the context switch,
#            compare-and-swap, the interrupt mask.
#   port/    this chip: starting core 1, the inter-processor interrupt that
#            parks it, the console, and the one-off heap carve at boot.
#
# A port to another chip replaces port/ and keeps the rest. A port to another
# architecture replaces arch/ as well.

idf_component_register(
    SRCS
        # --- the kernel ----------------------------------------------------
        "kernel/ksched.cpp"
        "kernel/ksync.cpp"
        "kernel/ktime.cpp"
        "kernel/kheap.cpp"
        "kernel/kmbox.cpp"
        "kernel/kfault.cpp"
        "kernel/kfmt.cpp"
        # --- the architecture ----------------------------------------------
        "arch/xtensa/kvectors.S"
        "arch/xtensa/kswitch.S"
        # --- this chip -----------------------------------------------------
        "port/kport.cpp"
        "port/kcore1.cpp"
        "port/kpark.cpp"
    INCLUDE_DIRS
        "."             # kernel/..., arch/..., port/...
        "../../include" # smartos.h and smartos_config.h, shared with src/
    REQUIRES
        esp_hw_support  # esp_cpu.h: core id, start and stall (port/kcore1.cpp)
        esp_rom         # esp_rom_sys.h: the ROM printf and delay (port/kport.h)
        esp_timer       # port/kpark.cpp times how long core 1 was parked
        freertos        # port/kport.cpp: kport_yield_ms, core 0 only
        hal             # hal/cpu_utility_ll.h (port/kcore1.cpp)
        soc             # soc/interrupts.h, soc/system_reg.h (port/kpark.cpp)
        xtensa          # xtensa/config/core-isa.h (arch/xtensa/kswitch.S)
)

# Stage 4.4: every SPI flash operation parks core 1. ESP-IDF calls these two
# around each one; --wrap sends those calls to the wrappers in port/kpark.cpp,
# which park core 1 and then call the originals. See the note in port/kpark.h.
#
# INTERFACE, so the flags reach the final link rather than stopping at this
# library. If they ever stop arriving, nothing fails to build - the park just
# quietly never happens, and -D SMARTOS_TEST_PARK is what says so.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=spi_flash_disable_interrupts_caches_and_other_cpu"
    "-Wl,--wrap=spi_flash_enable_interrupts_caches_and_other_cpu"
    # Stage 4.4: a restart puts core 1 to sleep first - see port/kcore1.cpp.
    "-Wl,--wrap=esp_restart_noos")
