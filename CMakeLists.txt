cmake_minimum_required(VERSION 3.20)
# LLVMConfig probes for libedit with check_include_file, which needs C enabled.
project(c-contracts C CXX)

# Out of tree, against a released clang. Nothing here needs a patched or forked
# compiler: point CMAKE_PREFIX_PATH at any LLVM install that ships
# ClangConfig.cmake (`brew --prefix llvm`, /usr/lib/llvm-N, ...).
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(c-contracts
  src/main.cpp
  src/Extract.cpp
  src/Ghost.cpp
)

target_include_directories(c-contracts SYSTEM PRIVATE
  ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
target_compile_definitions(c-contracts PRIVATE ${LLVM_DEFINITIONS})

# clang-cpp and LLVM are the single shared libraries every LLVM install ships;
# linking the fine-grained static libs instead would tie the build to how a
# particular distribution chose to slice them.
target_link_libraries(c-contracts PRIVATE clang-cpp LLVM)

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(c-contracts PRIVATE -fno-rtti)
endif()

enable_testing()
add_test(NAME suite
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run.sh $<TARGET_FILE:c-contracts>)
