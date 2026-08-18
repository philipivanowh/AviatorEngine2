cmake -B build -S . -DCMAKE_BUILD_TYPE=Release


dxc -T cs_6_0 -E main shader_comp.hlsl -Fo shader.comp.dxil
//Convert hlsl shader to metal
dxc -spirv -T cs_6_0 -E main shader.comp.hlsl -Fo shader.comp.spv
spirv-cross shader.comp.spv --msl --msl-version 20300 --output shader.comp.msl