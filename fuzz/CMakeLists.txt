# libFuzzer harnesses for everything that parses untrusted bytes. The input
# of a recovery tool is damaged by definition, so these are not optional.
#
#   CC=clang CXX=clang++ cmake -S . -B build-fuzz -DMCU_STUDIO_FUZZ=ON \
#       -DMCU_STUDIO_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build-fuzz --target fuzzers
#   ./build-fuzz/bin/fuzz_structure -max_total_time=60 fuzz/corpus
#
# The whole build is instrumented (see MCU_STUDIO_FUZZ in the top-level
# CMakeLists.txt); only the harnesses link libFuzzer's main.

set(MCU_FUZZERS structure decode bitstream splice exif project)
add_custom_target(fuzzers)
foreach(name IN LISTS MCU_FUZZERS)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE mcu_core)
    target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
    add_dependencies(fuzzers fuzz_${name})
endforeach()
