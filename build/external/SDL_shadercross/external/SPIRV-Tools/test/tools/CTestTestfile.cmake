# CMake generated Testfile for 
# Source directory: /Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools
# Build directory: /Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/SPIRV-Tools/test/tools
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("spirv-tools_expect_unittests" "/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "-m" "unittest" "expect_unittest.py")
set_tests_properties("spirv-tools_expect_unittests" PROPERTIES  WORKING_DIRECTORY "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools" _BACKTRACE_TRIPLES "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools/CMakeLists.txt;15;add_test;/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools/CMakeLists.txt;0;")
add_test("spirv-tools_spirv_test_framework_unittests" "/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "-m" "unittest" "spirv_test_framework_unittest.py")
set_tests_properties("spirv-tools_spirv_test_framework_unittests" PROPERTIES  WORKING_DIRECTORY "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools" _BACKTRACE_TRIPLES "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools/CMakeLists.txt;18;add_test;/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/test/tools/CMakeLists.txt;0;")
subdirs("opt")
subdirs("objdump")
