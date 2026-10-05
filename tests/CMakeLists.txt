find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(mcu_tests tst_core.cpp TestUtil.h)
target_link_libraries(mcu_tests PRIVATE mcu_core Qt6::Test JPEG::JPEG)
set_target_properties(mcu_tests PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
add_test(NAME mcu_tests COMMAND mcu_tests)
set_tests_properties(mcu_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
