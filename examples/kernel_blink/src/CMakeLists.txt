# The whole build for a program on the SmartOS kernel.
#
# Compare this with src/CMakeLists.txt: that one lists forty-odd sources and
# fifteen ESP-IDF components, because it is a whole operating system. This is
# what is left when you want only the kernel.
idf_component_register(
    SRCS "main.cpp"
    REQUIRES
        smartos         # the kernel, the architecture and the port
        esp_rom         # esp_rom_printf and the GPIO pad select, for the blink
        soc             # the GPIO registers the blink task writes
)
