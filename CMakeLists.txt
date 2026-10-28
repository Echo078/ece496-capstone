cmake_minimum_required(VERSION 3.22)
project(MixSense VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
add_library(MixSenseController STATIC
    plugin/Source/controller/ControllerBridge.cpp
    plugin/Source/controller/UsbFrameCodec.cpp
    plugin/Source/controller/SerialControllerTransport.cpp)
if(WIN32)
    target_sources(MixSenseController PRIVATE plugin/Source/controller/NativeSerialByteStream.cpp)
elseif(APPLE)
    target_sources(MixSenseController PRIVATE plugin/Source/controller/MacSerialByteStream.cpp)
else()
    target_sources(MixSenseController PRIVATE plugin/Source/controller/UnsupportedSerialByteStream.cpp)
    message(STATUS "Native controller serial I/O is unavailable on this platform; mocks remain usable")
endif()
target_link_libraries(MixSenseController PUBLIC Threads::Threads)
set_target_properties(MixSenseController PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(MixSenseController PUBLIC protocol plugin/Source/controller)

add_subdirectory(JUCE)

set(MIXSENSE_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND MIXSENSE_PLUGIN_FORMATS AU)
endif()

juce_add_plugin(MixSense
    COMPANY_NAME "MixSense"
    PLUGIN_MANUFACTURER_CODE Mxsn
    PLUGIN_CODE Mxs1
    FORMATS ${MIXSENSE_PLUGIN_FORMATS}
    PRODUCT_NAME "MixSense"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

target_sources(MixSense
    PRIVATE
        plugin/Source/PluginEditor.cpp
        plugin/Source/ControllerParameterAdapter.cpp
        plugin/Source/ContinuousFeedbackController.cpp
        plugin/Source/PluginProcessor.cpp)

target_include_directories(MixSense PRIVATE protocol)

target_compile_definitions(MixSense
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MixSense
    PRIVATE
        MixSenseController
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

include(CTest)
if(BUILD_TESTING)
    add_executable(MixSenseSerialPortTests tests/SerialPortNameTests.cpp)
    target_link_libraries(MixSenseSerialPortTests PRIVATE MixSenseController)
    add_test(NAME serial_port_names COMMAND MixSenseSerialPortTests)
    set_tests_properties(serial_port_names PROPERTIES TIMEOUT 10)
    if(APPLE)
        add_executable(MixSenseMacSerialTests tests/MacSerialByteStreamTests.cpp)
        target_link_libraries(MixSenseMacSerialTests PRIVATE MixSenseController)
        foreach(serial_case IN ITEMS configuration binary_io cleanup backpressure_hangup)
            add_test(NAME native_serial_macos_${serial_case} COMMAND MixSenseMacSerialTests ${serial_case})
            set_tests_properties(native_serial_macos_${serial_case} PROPERTIES TIMEOUT 10)
        endforeach()
    endif()
    add_executable(MixSenseControllerTests tests/ControllerTests.cpp)
    target_include_directories(MixSenseControllerTests PRIVATE plugin/Source JUCE/modules)
    target_link_libraries(MixSenseControllerTests PRIVATE MixSense MixSenseController)
    add_test(NAME controller_vertical_slice COMMAND MixSenseControllerTests)
    add_executable(MixSenseUsbTests tests/UsbControllerTests.cpp)
    target_include_directories(MixSenseUsbTests PRIVATE plugin/Source JUCE/modules)
    target_link_libraries(MixSenseUsbTests PRIVATE MixSense MixSenseController)
    add_test(NAME cdc_protocol_transport_integration COMMAND MixSenseUsbTests)
    add_executable(MixSenseRefinementTests tests/ControllerRefinementTests.cpp)
    target_include_directories(MixSenseRefinementTests PRIVATE plugin/Source JUCE/modules)
    target_link_libraries(MixSenseRefinementTests PRIVATE MixSense MixSenseController)
    add_test(NAME controller_refinement COMMAND MixSenseRefinementTests)
    set_tests_properties(controller_vertical_slice cdc_protocol_transport_integration controller_refinement PROPERTIES TIMEOUT 30)
    add_executable(MixSenseCompressorTests tests/CompressorTests.cpp)
    target_include_directories(MixSenseCompressorTests PRIVATE plugin/Source JUCE/modules)
    target_link_libraries(MixSenseCompressorTests PRIVATE MixSense MixSenseController)
    add_test(NAME compressor_dsp_and_meter COMMAND MixSenseCompressorTests)
    set_tests_properties(compressor_dsp_and_meter PROPERTIES TIMEOUT 30)
endif()
