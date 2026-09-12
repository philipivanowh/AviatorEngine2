cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

cmake --build build


dxc -spirv -T cs_6_0 -E main path_trace.comp.hlsl -Fo path_trace.comp.spv

dxc -spirv -T cs_6_0 -E main ray_trace.comp.hlsl -Fo ray_trace.comp.dxil
//Convert hlsl shader to metal
dxc -spirv -T cs_6_0 -E main shader.comp.hlsl -Fo shader.comp.spv
spirv-cross shader.comp.spv --msl --msl-version 20300 --output shader.comp.msl
