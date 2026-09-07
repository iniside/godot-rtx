#include "thirdparty/re-spirv/re-spirv.h"
#include <fstream>
#include <iterator>
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 4) return 1;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    respv::Shader shader(bytes.data(), bytes.size(), true);
    if (shader.empty()) return 2;
    std::ofstream inlined(argv[2], std::ios::binary);
    inlined.write(reinterpret_cast<const char *>(shader.inlinedSpirvWords.data()), shader.inlinedSpirvWords.size() * 4);
    std::vector<uint8_t> output;
    if (!respv::Optimizer::run(shader, nullptr, 0, output)) return 3;
    std::ofstream optimized(argv[3], std::ios::binary);
    optimized.write(reinterpret_cast<const char *>(output.data()), output.size());
    std::printf("input_bytes=%zu inlined_bytes=%zu optimized_bytes=%zu\n", bytes.size(), shader.inlinedSpirvWords.size() * 4, output.size());
    return inlined.good() && optimized.good() ? 0 : 4;
}
