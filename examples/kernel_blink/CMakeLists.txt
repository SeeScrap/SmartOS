# A SmartOS program, built as its own project.
#
# It is not part of the operating system's build and does not share its
# sdkconfig. The only thing it borrows is the component, by path - which is
# exactly how somebody else would use SmartOS in a project of their own.
#
# APPEND, do not set: PlatformIO has already put this project's own source
# directory in EXTRA_COMPONENT_DIRS by the time this file runs, and replacing
# the list loses it - the build then reports "Couldn't find the main target",
# which is a confusing way of saying the program itself went missing.
cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kernel_blink)
