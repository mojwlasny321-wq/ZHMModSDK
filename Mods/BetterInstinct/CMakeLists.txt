
add_library(BetterInstinct SHARED
    src/BetterInstinct.cpp
    src/BetterInstinct.h
)

target_include_directories(BetterInstinct PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(BetterInstinct ZHMModSDK winmm)
set_target_properties(BetterInstinct PROPERTIES CXX_STANDARD 23)
