# CMake generated Testfile for 
# Source directory: /Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools
# Build directory: /Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/SPIRV-Tools
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("spirv-tools-copyrights" "/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14" "utils/check_copyright.py")
set_tests_properties("spirv-tools-copyrights" PROPERTIES  WORKING_DIRECTORY "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools" _BACKTRACE_TRIPLES "/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/CMakeLists.txt;375;add_test;/Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/SPIRV-Tools/CMakeLists.txt;0;")
subdirs("external")
subdirs("source")
subdirs("tools")
subdirs("test")
subdirs("examples")
