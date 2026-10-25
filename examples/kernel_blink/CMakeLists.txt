# A SmartOS program, built as its own project.
#
# It is not part of the operating system's build and does not share its
# sdkconfig. The only thing it borrows is the component, by path - which is
# exactly how somebody else would use SmartOS in a project of their own.
cmake_minimum_required(VERSION 3.16.0)
set(EXTRA_COMPONENT_DIRS "../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kernel_blink)
