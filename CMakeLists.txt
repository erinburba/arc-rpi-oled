cmake_minimum_required(VERSION 3.16)
project(arc-rpi-oled LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(arc_oled
  src/ssd1306.cpp
  src/button_debounce.cpp
)
target_include_directories(arc_oled PUBLIC include)
target_compile_options(arc_oled PRIVATE -Wall -Wextra -Wpedantic)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)

  add_library(arc_hw
    src/spi_device.cpp
    src/i2c_device.cpp
    src/gpio_line.cpp
    src/button.cpp
    src/spi_display_transport.cpp
    src/i2c_display_transport.cpp
  )
  target_include_directories(arc_hw PUBLIC include)
  target_link_libraries(arc_hw PUBLIC arc_oled Threads::Threads)
  target_compile_options(arc_hw PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(oled_demo src/demo.cpp)
  target_link_libraries(oled_demo PRIVATE arc_hw)
  target_compile_options(oled_demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(ssd1306_test tests/ssd1306_test.cpp)
target_link_libraries(ssd1306_test PRIVATE arc_oled)
target_compile_options(ssd1306_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME ssd1306_test COMMAND ssd1306_test)

add_executable(button_test tests/button_test.cpp)
target_link_libraries(button_test PRIVATE arc_oled)
target_compile_options(button_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME button_test COMMAND button_test)
