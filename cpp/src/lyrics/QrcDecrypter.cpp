#include "lyrics/QrcDecrypter.h"

#include "util/Inflate.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace smtc::lyrics {
namespace {

// QRC 使用固定密钥的 3DES，再用 zlib 解压；下面这些类型让 DES 过程更明确。
using DesBlock = std::array<std::uint8_t, 8>;
using RoundKey = std::array<std::uint8_t, 6>;
using DesSchedule = std::array<RoundKey, 16>;
using TripleDesSchedule = std::array<DesSchedule, 3>;

constexpr int kEncrypt = 1;
constexpr int kDecrypt = 0;

// DES 的 8 个 S-Box，来自标准 DES 轮函数。
constexpr std::array<int, 64> kBox1{
    14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
    0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
    4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
    15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13
};

constexpr std::array<int, 64> kBox2{
    15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
    3, 13, 4, 7, 15, 2, 8, 15, 12, 0, 1, 10, 6, 9, 11, 5,
    0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
    13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9
};

constexpr std::array<int, 64> kBox3{
    10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
    13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
    13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
    1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12
};

constexpr std::array<int, 64> kBox4{
    7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
    13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
    10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
    3, 15, 0, 6, 10, 10, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14
};

constexpr std::array<int, 64> kBox5{
    2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
    14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
    4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
    11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3
};

constexpr std::array<int, 64> kBox6{
    12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
    10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
    9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
    4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13
};

constexpr std::array<int, 64> kBox7{
    4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
    13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
    1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
    6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12
};

constexpr std::array<int, 64> kBox8{
    13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
    1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
    7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
    2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11
};

constexpr std::array<int, 16> kKeyRndShift{1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

// DES 密钥置换和压缩置换表。
constexpr std::array<int, 28> kKeyPermC{
    56, 48, 40, 32, 24, 16, 8, 0, 57, 49, 41, 33, 25, 17,
    9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35
};

constexpr std::array<int, 28> kKeyPermD{
    62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21,
    13, 5, 60, 52, 44, 36, 28, 20, 12, 4, 27, 19, 11, 3
};

constexpr std::array<int, 48> kKeyCompression{
    13, 16, 10, 23, 0, 4, 2, 27, 14, 5, 20, 9, 22, 18,
    11, 3, 25, 7, 15, 6, 26, 19, 12, 1, 40, 51, 30, 36,
    46, 54, 29, 39, 50, 44, 32, 47, 43, 48, 38, 55, 33,
    52, 45, 41, 49, 35, 28, 31
};

// 从字节数组中取某一位，并移动到目标 bit 位置。
std::uint32_t getBitFromByteArray(const std::uint8_t* bytes, int bit, int targetShift) {
    const int index = bit / 32 * 4 + 3 - (bit % 32) / 8;
    const auto byteValue = static_cast<std::uint32_t>(bytes[index]);
    return ((byteValue >> (7 - (bit % 8))) & 0x01u) << targetShift;
}

// 从 32 位整数中按 DES 约定取位，分别服务左右两种排列写法。
std::uint32_t getBitFromIntR(std::uint32_t value, int bit, int targetShift) {
    return ((value >> (31 - bit)) & 0x01u) << targetShift;
}

std::uint32_t getBitFromIntL(std::uint32_t value, int bit, int targetShift) {
    return ((value << bit) & 0x80000000u) >> targetShift;
}

std::size_t formatSBoxInput(std::uint32_t value) {
    // S-Box 索引由首尾两位作为行，中间四位作为列。
    return static_cast<std::size_t>((value & 0x20u) | ((value & 0x1fu) >> 1) | ((value & 0x01u) << 4));
}

void keySchedule(const std::array<std::uint8_t, 24>& key, int offset, DesSchedule& schedule, int mode) {
    // 从 24 字节 3DES key 中取一个 8 字节子密钥并生成 16 轮轮密钥。
    std::uint32_t c = 0;
    std::uint32_t d = 0;
    for (int i = 0; i < 28; ++i) {
        c |= getBitFromByteArray(key.data(), kKeyPermC[static_cast<std::size_t>(i)] + offset * 8, 31 - i);
        d |= getBitFromByteArray(key.data(), kKeyPermD[static_cast<std::size_t>(i)] + offset * 8, 31 - i);
    }

    for (int i = 0; i < 16; ++i) {
        const int shift = kKeyRndShift[static_cast<std::size_t>(i)];
        c = ((c << shift) | (c >> (28 - shift))) & 0xFFFFFFF0u;
        d = ((d << shift) | (d >> (28 - shift))) & 0xFFFFFFF0u;
        const int generatedIndex = mode == kDecrypt ? 15 - i : i;
        // 解密时轮密钥顺序反向。
        auto& round = schedule[static_cast<std::size_t>(generatedIndex)];
        round.fill(0);
        for (int k = 0; k < 24; ++k) {
            round[static_cast<std::size_t>(k / 8)] = static_cast<std::uint8_t>(
                round[static_cast<std::size_t>(k / 8)] |
                getBitFromIntR(c, kKeyCompression[static_cast<std::size_t>(k)], 7 - (k % 8)));
        }
        for (int k = 24; k < 48; ++k) {
            round[static_cast<std::size_t>(k / 8)] = static_cast<std::uint8_t>(
                round[static_cast<std::size_t>(k / 8)] |
                getBitFromIntR(d, kKeyCompression[static_cast<std::size_t>(k)] - 27, 7 - (k % 8)));
        }
    }
}

TripleDesSchedule makeDecryptSchedule() {
    // QQ QRC 的固定 3DES 密钥，按“解密-加密-解密”顺序生成调度表。
    constexpr std::array<std::uint8_t, 24> kQqKey{
        '!', '@', '#', ')', '(', '*', '$', '%',
        '1', '2', '3', 'Z', 'X', 'C', '!', '@',
        '!', '@', '#', ')', '(', 'N', 'H', 'L'
    };

    TripleDesSchedule schedule{};
    keySchedule(kQqKey, 0, schedule[2], kDecrypt);
    keySchedule(kQqKey, 8, schedule[1], kEncrypt);
    keySchedule(kQqKey, 16, schedule[0], kDecrypt);
    return schedule;
}

void initialPermutation(std::array<std::uint32_t, 2>& state, const DesBlock& input) {
    // DES 初始置换，把 64 位输入拆成左右两个 32 位状态。
    constexpr std::array<int, 32> kIp0{
        57, 49, 41, 33, 25, 17, 9, 1,
        59, 51, 43, 35, 27, 19, 11, 3,
        61, 53, 45, 37, 29, 21, 13, 5,
        63, 55, 47, 39, 31, 23, 15, 7
    };
    constexpr std::array<int, 32> kIp1{
        56, 48, 40, 32, 24, 16, 8, 0,
        58, 50, 42, 34, 26, 18, 10, 2,
        60, 52, 44, 36, 28, 20, 12, 4,
        62, 54, 46, 38, 30, 22, 14, 6
    };

    state = {};
    for (int i = 0; i < 32; ++i) {
        state[0] |= getBitFromByteArray(input.data(), kIp0[static_cast<std::size_t>(i)], 31 - i);
        state[1] |= getBitFromByteArray(input.data(), kIp1[static_cast<std::size_t>(i)], 31 - i);
    }
}

void inverseInitialPermutation(const std::array<std::uint32_t, 2>& state, DesBlock& output) {
    // DES 结束置换，把左右状态重新打包回 8 字节。
    constexpr std::array<int, 8> kOutIndices{3, 2, 1, 0, 7, 6, 5, 4};
    constexpr std::array<int, 8> kBitOffsets{7, 6, 5, 4, 3, 2, 1, 0};

    for (int i = 0; i < 8; ++i) {
        const int bitOffset = kBitOffsets[static_cast<std::size_t>(i)];
        output[static_cast<std::size_t>(kOutIndices[static_cast<std::size_t>(i)])] = static_cast<std::uint8_t>(
            getBitFromIntR(state[1], bitOffset, 7) |
            getBitFromIntR(state[0], bitOffset, 6) |
            getBitFromIntR(state[1], bitOffset + 8, 5) |
            getBitFromIntR(state[0], bitOffset + 8, 4) |
            getBitFromIntR(state[1], bitOffset + 16, 3) |
            getBitFromIntR(state[0], bitOffset + 16, 2) |
            getBitFromIntR(state[1], bitOffset + 24, 1) |
            getBitFromIntR(state[0], bitOffset + 24, 0));
    }
}

std::uint32_t f(std::uint32_t stateIn, const RoundKey& key) {
    // DES Feistel 轮函数：扩展、异或轮密钥、S-Box、P 置换。
    const std::uint32_t t1 =
        getBitFromIntL(stateIn, 31, 0) | ((stateIn & 0xF0000000u) >> 1) |
        getBitFromIntL(stateIn, 4, 5) | getBitFromIntL(stateIn, 3, 6) |
        ((stateIn & 0x0F000000u) >> 3) |
        getBitFromIntL(stateIn, 8, 11) | getBitFromIntL(stateIn, 7, 12) |
        ((stateIn & 0x00F00000u) >> 5) |
        getBitFromIntL(stateIn, 12, 17) | getBitFromIntL(stateIn, 11, 18) |
        ((stateIn & 0x000F0000u) >> 7) | getBitFromIntL(stateIn, 16, 23);

    const std::uint32_t t2 =
        getBitFromIntL(stateIn, 15, 0) | ((stateIn & 0x0000F000u) << 15) |
        getBitFromIntL(stateIn, 20, 5) | getBitFromIntL(stateIn, 19, 6) |
        ((stateIn & 0x00000F00u) << 13) |
        getBitFromIntL(stateIn, 24, 11) | getBitFromIntL(stateIn, 23, 12) |
        ((stateIn & 0x000000F0u) << 11) |
        getBitFromIntL(stateIn, 28, 17) | getBitFromIntL(stateIn, 27, 18) |
        ((stateIn & 0x0000000Fu) << 9) | getBitFromIntL(stateIn, 0, 23);

    const std::uint32_t x0 = ((t1 >> 24) & 0xFFu) ^ key[0];
    const std::uint32_t x1 = ((t1 >> 16) & 0xFFu) ^ key[1];
    const std::uint32_t x2 = ((t1 >> 8) & 0xFFu) ^ key[2];
    const std::uint32_t x3 = ((t2 >> 24) & 0xFFu) ^ key[3];
    const std::uint32_t x4 = ((t2 >> 16) & 0xFFu) ^ key[4];
    const std::uint32_t x5 = ((t2 >> 8) & 0xFFu) ^ key[5];

    const std::uint32_t state =
        (static_cast<std::uint32_t>(kBox1[formatSBoxInput(x0 >> 2)]) << 28) |
        (static_cast<std::uint32_t>(kBox2[formatSBoxInput(((x0 & 0x03u) << 4) | (x1 >> 4))]) << 24) |
        (static_cast<std::uint32_t>(kBox3[formatSBoxInput(((x1 & 0x0Fu) << 2) | (x2 >> 6))]) << 20) |
        (static_cast<std::uint32_t>(kBox4[formatSBoxInput(x2 & 0x3Fu)]) << 16) |
        (static_cast<std::uint32_t>(kBox5[formatSBoxInput(x3 >> 2)]) << 12) |
        (static_cast<std::uint32_t>(kBox6[formatSBoxInput(((x3 & 0x03u) << 4) | (x4 >> 4))]) << 8) |
        (static_cast<std::uint32_t>(kBox7[formatSBoxInput(((x4 & 0x0Fu) << 2) | (x5 >> 6))]) << 4) |
        static_cast<std::uint32_t>(kBox8[formatSBoxInput(x5 & 0x3Fu)]);

    return
        getBitFromIntL(state, 15, 0) | getBitFromIntL(state, 6, 1) |
        getBitFromIntL(state, 19, 2) | getBitFromIntL(state, 20, 3) |
        getBitFromIntL(state, 28, 4) | getBitFromIntL(state, 11, 5) |
        getBitFromIntL(state, 27, 6) | getBitFromIntL(state, 16, 7) |
        getBitFromIntL(state, 0, 8) | getBitFromIntL(state, 14, 9) |
        getBitFromIntL(state, 22, 10) | getBitFromIntL(state, 25, 11) |
        getBitFromIntL(state, 4, 12) | getBitFromIntL(state, 17, 13) |
        getBitFromIntL(state, 30, 14) | getBitFromIntL(state, 9, 15) |
        getBitFromIntL(state, 1, 16) | getBitFromIntL(state, 7, 17) |
        getBitFromIntL(state, 23, 18) | getBitFromIntL(state, 13, 19) |
        getBitFromIntL(state, 31, 20) | getBitFromIntL(state, 26, 21) |
        getBitFromIntL(state, 2, 22) | getBitFromIntL(state, 8, 23) |
        getBitFromIntL(state, 18, 24) | getBitFromIntL(state, 12, 25) |
        getBitFromIntL(state, 29, 26) | getBitFromIntL(state, 5, 27) |
        getBitFromIntL(state, 21, 28) | getBitFromIntL(state, 10, 29) |
        getBitFromIntL(state, 3, 30) | getBitFromIntL(state, 24, 31);
}

void crypt(const DesBlock& input, DesBlock& output, const DesSchedule& key) {
    // 单 DES 加/解密；差异已经体现在 keySchedule 的轮密钥顺序里。
    std::array<std::uint32_t, 2> state{};
    initialPermutation(state, input);
    for (int index = 0; index < 15; ++index) {
        const auto t = state[1];
        state[1] = f(state[1], key[static_cast<std::size_t>(index)]) ^ state[0];
        state[0] = t;
    }
    state[0] = f(state[1], key[15]) ^ state[0];
    inverseInitialPermutation(state, output);
}

void tripleDesCrypt(const DesBlock& input, DesBlock& output, const TripleDesSchedule& key) {
    // 3DES 由三个单 DES 阶段串联。
    crypt(input, output, key[0]);
    crypt(output, output, key[1]);
    crypt(output, output, key[2]);
}

int hexValue(char ch) {
    // QRC 接口返回十六进制字符串，需要先转字节再解密。
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::vector<std::uint8_t> hexToBytes(std::string_view hex) {
    // 允许密文里带少量空白；出现其他非法字符则整体失败。
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    int high = -1;
    for (char ch : hex) {
        const int value = hexValue(ch);
        if (value < 0) {
            if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
            return {};
        }
        if (high < 0) {
            high = value;
        } else {
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | value));
            high = -1;
        }
    }

    if (high >= 0) return {};
    return bytes;
}

}

std::string decryptQrc(std::string_view encryptedHex) {
    // 外部入口：十六进制 -> 3DES 解密 -> zlib 解压 -> UTF-8 文本。
    const auto encryptedBytes = hexToBytes(encryptedHex);
    if (encryptedBytes.empty()) return {};

    static const TripleDesSchedule schedule = makeDecryptSchedule();
    std::vector<std::uint8_t> decryptedData(encryptedBytes.size());
    DesBlock inputBlock{};
    DesBlock outputBlock{};

    for (std::size_t i = 0; i < encryptedBytes.size(); i += inputBlock.size()) {
        // 最后一块不足 8 字节时只拷贝实际长度，保持与原实现兼容。
        const auto blockSize = std::min(inputBlock.size(), encryptedBytes.size() - i);
        std::copy_n(encryptedBytes.data() + i, blockSize, inputBlock.data());
        tripleDesCrypt(inputBlock, outputBlock, schedule);
        std::copy_n(outputBlock.data(), blockSize, decryptedData.data() + i);
    }

    const auto inflated = util::inflateZlib(decryptedData);
    return {inflated.begin(), inflated.end()};
}

}
