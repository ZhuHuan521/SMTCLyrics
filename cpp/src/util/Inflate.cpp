#include "util/Inflate.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace smtc::util {
namespace {

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint32_t readBits(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value |= readBit() << i;
        }
        return value;
    }

    void alignByte() {
        bitPos_ = (bitPos_ + 7) & ~std::size_t{7};
    }

private:
    std::uint32_t readBit() {
        if (bitPos_ >= size_ * 8) throw std::runtime_error("unexpected end of deflate stream");
        const auto byte = data_[bitPos_ / 8];
        const auto bit = static_cast<std::uint32_t>((byte >> (bitPos_ % 8)) & 1);
        ++bitPos_;
        return bit;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bitPos_ = 0;
};

std::uint32_t reverseBits(std::uint32_t code, int length) {
    std::uint32_t result = 0;
    for (int i = 0; i < length; ++i) {
        result = (result << 1) | (code & 1);
        code >>= 1;
    }
    return result;
}

class HuffmanTree {
public:
    void build(const std::vector<int>& lengths) {
        entries_.clear();
        maxBits_ = 0;
        for (int length : lengths) {
            maxBits_ = std::max(maxBits_, length);
        }
        if (maxBits_ <= 0) throw std::runtime_error("empty huffman tree");

        std::vector<int> counts(static_cast<std::size_t>(maxBits_ + 1), 0);
        for (int length : lengths) {
            if (length < 0) throw std::runtime_error("invalid huffman length");
            if (length > 0) ++counts[static_cast<std::size_t>(length)];
        }

        std::vector<int> nextCode(static_cast<std::size_t>(maxBits_ + 1), 0);
        int code = 0;
        for (int bits = 1; bits <= maxBits_; ++bits) {
            code = (code + counts[static_cast<std::size_t>(bits - 1)]) << 1;
            nextCode[static_cast<std::size_t>(bits)] = code;
        }

        for (int symbol = 0; symbol < static_cast<int>(lengths.size()); ++symbol) {
            const int length = lengths[static_cast<std::size_t>(symbol)];
            if (length == 0) continue;
            const auto canonical = static_cast<std::uint32_t>(nextCode[static_cast<std::size_t>(length)]++);
            entries_.push_back({symbol, length, reverseBits(canonical, length)});
        }
    }

    int decode(BitReader& reader) const {
        std::uint32_t code = 0;
        for (int length = 1; length <= maxBits_; ++length) {
            code |= reader.readBits(1) << (length - 1);
            for (const auto& entry : entries_) {
                if (entry.length == length && entry.code == code) {
                    return entry.symbol;
                }
            }
        }
        throw std::runtime_error("invalid huffman code");
    }

private:
    struct Entry {
        int symbol = 0;
        int length = 0;
        std::uint32_t code = 0;
    };

    std::vector<Entry> entries_;
    int maxBits_ = 0;
};

HuffmanTree fixedLiteralTree() {
    std::vector<int> lengths(288, 0);
    for (int i = 0; i <= 143; ++i) lengths[static_cast<std::size_t>(i)] = 8;
    for (int i = 144; i <= 255; ++i) lengths[static_cast<std::size_t>(i)] = 9;
    for (int i = 256; i <= 279; ++i) lengths[static_cast<std::size_t>(i)] = 7;
    for (int i = 280; i <= 287; ++i) lengths[static_cast<std::size_t>(i)] = 8;
    HuffmanTree tree;
    tree.build(lengths);
    return tree;
}

HuffmanTree fixedDistanceTree() {
    HuffmanTree tree;
    tree.build(std::vector<int>(32, 5));
    return tree;
}

void decodeCompressedBlock(BitReader& reader, const HuffmanTree& literalTree, const HuffmanTree& distanceTree, std::vector<std::uint8_t>& output) {
    static constexpr int kLengthBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistanceBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr int kDistanceExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        const int symbol = literalTree.decode(reader);
        if (symbol < 256) {
            output.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return;
        } else if (symbol >= 257 && symbol <= 285) {
            const int lengthIndex = symbol - 257;
            int length = kLengthBase[lengthIndex] + static_cast<int>(reader.readBits(kLengthExtra[lengthIndex]));
            const int distanceSymbol = distanceTree.decode(reader);
            if (distanceSymbol < 0 || distanceSymbol >= 30) throw std::runtime_error("invalid deflate distance");
            int distance = kDistanceBase[distanceSymbol] + static_cast<int>(reader.readBits(kDistanceExtra[distanceSymbol]));
            if (distance <= 0 || static_cast<std::size_t>(distance) > output.size()) throw std::runtime_error("invalid deflate back-reference");

            for (int i = 0; i < length; ++i) {
                output.push_back(output[output.size() - static_cast<std::size_t>(distance)]);
            }
        } else {
            throw std::runtime_error("invalid deflate literal symbol");
        }
    }
}

void decodeDynamicBlock(BitReader& reader, std::vector<std::uint8_t>& output) {
    static constexpr int kCodeLengthOrder[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

    const int literalCount = static_cast<int>(reader.readBits(5)) + 257;
    const int distanceCount = static_cast<int>(reader.readBits(5)) + 1;
    const int codeLengthCount = static_cast<int>(reader.readBits(4)) + 4;

    std::vector<int> codeLengths(19, 0);
    for (int i = 0; i < codeLengthCount; ++i) {
        codeLengths[static_cast<std::size_t>(kCodeLengthOrder[i])] = static_cast<int>(reader.readBits(3));
    }

    HuffmanTree codeLengthTree;
    codeLengthTree.build(codeLengths);

    std::vector<int> lengths;
    lengths.reserve(static_cast<std::size_t>(literalCount + distanceCount));
    while (static_cast<int>(lengths.size()) < literalCount + distanceCount) {
        const int symbol = codeLengthTree.decode(reader);
        if (symbol <= 15) {
            lengths.push_back(symbol);
        } else if (symbol == 16) {
            if (lengths.empty()) throw std::runtime_error("invalid repeated huffman length");
            const int repeat = static_cast<int>(reader.readBits(2)) + 3;
            lengths.insert(lengths.end(), repeat, lengths.back());
        } else if (symbol == 17) {
            const int repeat = static_cast<int>(reader.readBits(3)) + 3;
            lengths.insert(lengths.end(), repeat, 0);
        } else if (symbol == 18) {
            const int repeat = static_cast<int>(reader.readBits(7)) + 11;
            lengths.insert(lengths.end(), repeat, 0);
        } else {
            throw std::runtime_error("invalid code length symbol");
        }
    }
    lengths.resize(static_cast<std::size_t>(literalCount + distanceCount));

    HuffmanTree literalTree;
    literalTree.build(std::vector<int>(lengths.begin(), lengths.begin() + literalCount));
    HuffmanTree distanceTree;
    distanceTree.build(std::vector<int>(lengths.begin() + literalCount, lengths.end()));
    decodeCompressedBlock(reader, literalTree, distanceTree, output);
}

std::vector<std::uint8_t> inflateDeflate(const std::uint8_t* data, std::size_t size) {
    BitReader reader(data, size);
    std::vector<std::uint8_t> output;
    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = reader.readBits(1) != 0;
        const auto blockType = reader.readBits(2);
        if (blockType == 0) {
            reader.alignByte();
            const auto len = static_cast<std::uint16_t>(reader.readBits(16));
            const auto nlen = static_cast<std::uint16_t>(reader.readBits(16));
            if (static_cast<std::uint16_t>(len ^ 0xFFFF) != nlen) throw std::runtime_error("invalid stored deflate block");
            for (std::uint16_t i = 0; i < len; ++i) {
                output.push_back(static_cast<std::uint8_t>(reader.readBits(8)));
            }
        } else if (blockType == 1) {
            decodeCompressedBlock(reader, fixedLiteralTree(), fixedDistanceTree(), output);
        } else if (blockType == 2) {
            decodeDynamicBlock(reader, output);
        } else {
            throw std::runtime_error("reserved deflate block type");
        }
    }
    return output;
}

}

std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& bytes) {
    try {
        if (bytes.size() < 2) return {};
        const auto cmf = bytes[0];
        const auto flg = bytes[1];
        if ((cmf & 0x0F) != 8) return {};
        if ((((static_cast<int>(cmf) << 8) + flg) % 31) != 0) return {};
        if ((flg & 0x20) != 0) return {};
        return inflateDeflate(bytes.data() + 2, bytes.size() - 2);
    } catch (...) {
        return {};
    }
}

}
