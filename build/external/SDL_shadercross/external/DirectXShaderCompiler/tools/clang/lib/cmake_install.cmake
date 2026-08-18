# Install script for directory: /Users/lansmachine/Documents/Github/AviatorEngine2/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Debug")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Basic/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Lex/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Parse/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/AST/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/ASTMatchers/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Sema/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/CodeGen/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Analysis/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Edit/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Rewrite/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Driver/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Frontend/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/FrontendTool/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Tooling/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Index/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/Format/cmake_install.cmake")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/SPIRV/cmake_install.cmake")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lansmachine/Documents/Github/AviatorEngine2/build/external/SDL_shadercross/external/DirectXShaderCompiler/tools/clang/lib/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
