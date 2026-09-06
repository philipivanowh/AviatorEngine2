// The single translation unit that compiles stb_image itself.
//
// texture.h used to carry STB_IMAGE_IMPLEMENTATION, which was fine while
// main.cpp was the only .cpp - now every translation unit that reaches
// texture.h would emit its own copy of every stbi_* function and the link
// fails. The header declares, this file defines.

#define STB_IMAGE_IMPLEMENTATION
#include "external/stb_image.h"
