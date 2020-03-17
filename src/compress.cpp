#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <map>
#include <string>
#include <vector>
#include <list>

#define panic(fmt, ...)                                   \
    do {                                                  \
        fprintf(stderr, "ERR: " fmt "\n", ##__VA_ARGS__); \
        exit(1);                                          \
    } while (0)

#define xassert(c, fmt, ...)                                     \
    do {                                                         \
        auto r = (c);                                            \
        if (!r) {                                                \
            fprintf(stderr, "ASSERT: " fmt "\n", ##__VA_ARGS__); \
            assert(0);                                           \
        }                                                        \
    } while (0)

#define DEBUG0(msg) fprintf(stderr, "DEBUG: " msg "\n");
#define DEBUG(fmt, ...) fprintf(stderr, "DEBUG: " fmt "\n", ##__VA_ARGS__);

#define ARRSIZE(x) (sizeof(x) / sizeof(x[0]))

constexpr size_t BUFSIZE  = 2056;
constexpr size_t READSIZE = 2056;
constexpr size_t BLOCKSIZE = 2056;
constexpr uint16_t EmptySentinel = UINT16_MAX;
constexpr size_t NumHeaderCodeLengths = 19;
constexpr size_t LiteralCodes = 256; // doesn't include END_BLOCK code
constexpr size_t LengthCodes = 29;
constexpr size_t LitCodes = LiteralCodes + LengthCodes + 1;
constexpr size_t DistCodes = 30;
constexpr size_t MaxNumCodes = LitCodes + DistCodes;
constexpr size_t HeaderLengthBits = 3;
constexpr size_t MaxHeaderCodeLength = 1u << HeaderLengthBits;

// TODO: switch to MaxNumCodes
// TODO: can definitely reduce this -- but do need it to work with fixed huffman? unless i'm just going to use the generated versions
constexpr size_t MaxCodes = 512;
constexpr size_t MaxBits = 15;
constexpr size_t HLIT = 258;
constexpr size_t HDIST = 10;

// TEMP TEMP
constexpr uint16_t _codelens[HLIT + HDIST] = {
    // HLIT
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 6, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    6, 5, 6, 6, 5, 5, 6, 5,
    6, 5, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 5, 5, 6, 5, 5, 5, 5,
    5, 6, 5, 5, 5, 5, 5, 6,
    5, 6, 6, 5, 5, 6, 5, 5,
    5, 5, 5, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    6, 6,
    // HDIST
    1, 0, 0, 0, 0, 0, 0, 0,
    0, 1,
};

uint32_t crc_table[256];

void init_crc_table() {
    for (int n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

uint32_t calc_crc32(uint32_t crc, const char* buf, size_t len) {
    if (buf == NULL) return 0;
    crc = crc ^ 0xffffffffUL;
    for (size_t i = 0; i < len; ++i) {
        crc = crc_table[((int)crc ^ (*buf++)) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffUL;
}

uint8_t ID1_GZIP = 31;
uint8_t ID2_GZIP = 139;
uint8_t CM_DEFLATE = 8;

struct FileHandle {
    FileHandle(FILE* f = nullptr) noexcept : fp(f) {}
    FileHandle(FileHandle&) noexcept = delete;
    FileHandle& operator=(FileHandle&) noexcept = delete;
    FileHandle(FileHandle&& rhs) noexcept : fp(rhs.fp) { rhs.fp = nullptr; }
    FileHandle& operator=(FileHandle&& rhs) noexcept {
        fp = rhs.fp;
        rhs.fp = nullptr;
        return *this;
    }
    ~FileHandle() noexcept {
        if (fp) {
            fclose(fp);
        }
    }
    operator FILE*() noexcept { return fp; }
    explicit operator bool() const noexcept { return fp != nullptr; }
    FILE* fp;
};

enum class Flags : uint8_t {
    FTEXT = 1u << 0,
    FHCRC = 1u << 1,
    FEXTRA = 1u << 2,
    FNAME = 1u << 3,
    FCOMMENT = 1u << 4,
    RESERV1 = 1u << 5,
    RESERV2 = 1u << 6,
    RESERV3 = 1u << 7,
};

// BTYPE specifies how the data are compressed, as follows:
// 00 - no compression
// 01 - compressed with fixed Huffman codes
// 10 - compressed with dynamic Huffman codes
// 11 - reserved (error)
enum class BType : uint8_t {
    NO_COMPRESSION = 0x0u,
    FIXED_HUFFMAN = 0x1u,
    DYNAMIC_HUFFMAN = 0x2u,
    RESERVED = 0x3u,
};

void xwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    if (fwrite(ptr, size, nmemb, stream) != nmemb) {
        panic("short write");
    }
}

struct Code {
    constexpr Code(uint16_t code_, uint16_t bits_) noexcept : code{code_}, bits{bits_} {}
    uint16_t code;
    uint16_t bits;
};

using Value = uint16_t;
using Tree = std::map<Value, Code>;
using CodeLengths = std::vector<uint16_t>;

// TODO: use Code
struct TreeNode {
    TreeNode(int v, int b) noexcept : value{v}, bits{b} {}
    int value;
    int bits;
};

struct Node {
    Node(int v, int w) : value{v}, weight{w} {}
    const int value;
    const int weight;
    const Node* left = nullptr;
    const Node* right = nullptr;
    mutable int depth = -1;
};

struct NodeCmp {
    bool operator()(const Node* a, const Node* b) {
        // STL heap api is for a max heap and expects less than comparison
        return a->weight > b->weight;
    }
};

void assign_depth(const Node* n, int depth) {
    if (!n)
        return;
    assign_depth(n->left, depth+1);
    n->depth = depth;
    // printf("n: value=%d weight=%d depth=%d\n", n->value, n->weight, depth);
    assign_depth(n->right, depth+1);
}

std::vector<TreeNode> construct_huffman_tree(const int* codelens, size_t n_codelens) {
    std::map<int, int> counts;
    for (size_t i = 0; i < n_codelens; ++i) {
        counts[codelens[i]]++;
    }

    // TODO: max number of nodes is 2*N + 1?
    printf("--- COUNTS ---\n");
    std::list<Node> pool;
    std::vector<Node*> nodes;
    for (auto&& [codelen, count] : counts) {
        auto& n = pool.emplace_back(codelen, count);
        n.left = n.right = nullptr;
        nodes.push_back(&n);
        printf("%d: %d\n", codelen, count);
    }
    printf("--- END COUNTS ---\n");

    auto&& pop_heap = [](std::vector<Node*>& nodes) {
        std::pop_heap(nodes.begin(), nodes.end(), NodeCmp{});
        nodes.pop_back();
    };

    std::make_heap(nodes.begin(), nodes.end(), NodeCmp{});
    while (nodes.size() >= 2) {
        Node* a = nodes[0];
        pop_heap(nodes);
        Node* b = nodes[0];
        pop_heap(nodes);
        auto& n = pool.emplace_back(-1, a->weight + b->weight);
        n.left  = a->weight < b->weight ? a : b;
        n.right = a->weight < b->weight ? b : a;
        nodes.push_back(&n);
        std::push_heap(nodes.begin(), nodes.end(), NodeCmp{});
    }

    assert(nodes.size() == 1);
    assign_depth(nodes[0], 0);

    printf("--- PRINT NODES ---\n");
    for (auto&& node : pool) {
        printf("(%d, %d) ", node.value, node.depth);
    }
    printf("\n");
    printf("--- END PRINT NODES ---\n");

    std::vector<TreeNode> result;
    for (auto&& node : pool) {
        if (node.value != -1) {
            result.emplace_back(node.value, node.depth);
        }
    }
    return result;
}

struct DynamicCodeLengths {
    CodeLengths codelens;
    size_t      hlit;
    size_t      hdist;
};

// TODO: need to pass in data
DynamicCodeLengths make_code_lengths() {
    CodeLengths result;
    result.assign(&_codelens[0], &_codelens[ARRSIZE(_codelens)]);
    // result.insert(result.end(), &_codelens[0], &_codelens[HLIT]);
    // result.insert(result.end(), &_codelens[HLIT], &_codelens[HDIST]);
    return { result, HLIT, HDIST };
}

static const unsigned char BitReverseTable256[256] = {
// clang-format off
#   define R2(n)     n,     n + 2*64,     n + 1*64,     n + 3*64
#   define R4(n) R2(n), R2(n + 2*16), R2(n + 1*16), R2(n + 3*16)
#   define R6(n) R4(n), R4(n + 2*4 ), R4(n + 1*4 ), R4(n + 3*4 )
    R6(0), R6(2), R6(1), R6(3)
#undef R2
#undef R4
#undef R6
    // clang-format on
};

uint16_t flip_u16(uint16_t v) noexcept {
    // clang-format off
    return static_cast<uint16_t>(
        (BitReverseTable256[(v >> 0) & 0xff] << 8) |
        (BitReverseTable256[(v >> 8) & 0xff] << 0)
    );
    // clang-format on
}

uint16_t flip_code(uint16_t code, size_t codelen) {
    assert(0 < codelen && codelen <= 16);
    return static_cast<uint16_t>(flip_u16(code) >> (16 - codelen));
}

Tree init_huffman_tree(const uint16_t* code_lengths, size_t n) {
    size_t bl_count[MaxBits];
    uint16_t next_code[MaxBits];
    uint16_t codes[MaxCodes];

    if (!(n < MaxCodes)) {
        throw std::runtime_error{"too many code lengths"};
    }

    // 1) Count the number of codes for each code length. Let bl_count[N] be the
    // number of codes of length N, N >= 1.
    memset(&bl_count[0], 0, sizeof(bl_count));
    size_t max_bit_length = 0;
    for (size_t i = 0; i < n; ++i) {
        xassert(code_lengths[i] <= MaxBits, "Unsupported bit length");
        ++bl_count[code_lengths[i]];
        max_bit_length = std::max<uint16_t>(code_lengths[i], max_bit_length);
    }
    bl_count[0] = 0;

    // 2) Find the numerical value of the smallest code for each code length:
    memset(&next_code[0], 0, sizeof(next_code));
    uint32_t code = 0;
    for (size_t bits = 1; bits <= max_bit_length; ++bits) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    // 3) Assign numerical values to all codes, using consecutive values for all
    // codes of the same length with the base values determined at step 2. Codes
    // that are never used (which have a bit length of zero) must not be
    // assigned a value.
    memset(&codes[0], 0, sizeof(codes));
    for (size_t i = 0; i < n; ++i) {
        if (code_lengths[i] != 0) {
            codes[i] = next_code[code_lengths[i]]++;
            xassert((16 - __builtin_clz(codes[i])) <= code_lengths[i], "overflowed code length");
        }
    }

    Tree tree;
    auto n_values = static_cast<uint16_t>(n);
    for (uint16_t value = 0; value < n_values; ++value) {
        uint16_t code_length = code_lengths[value];
        if (code_length == 0) {
            continue;
        }
        uint16_t code = codes[value];
        tree.emplace(value, Code{flip_code(code, code_length), code_length});
        // tree.emplace(value, Code{code, code_length});
    }

    return tree;
}

std::pair<Tree, Tree> init_fixed_huffman_data() noexcept {
    std::vector<uint16_t> lit_codes;
    while (lit_codes.size() < 144) {
        lit_codes.push_back(8);
    }
    while (lit_codes.size() < 256) {
        lit_codes.push_back(9);
    }
    while (lit_codes.size() < 280) {
        lit_codes.push_back(7);
    }
    while (lit_codes.size() < 288) {
        lit_codes.push_back(8);
    }
    assert(lit_codes.size() == 288);
    auto lit_tree = init_huffman_tree(lit_codes.data(), lit_codes.size());

    std::vector<uint16_t> dst_codes(32, 5);
    assert(dst_codes.size() == 32);
    auto dst_tree = init_huffman_tree(dst_codes.data(), dst_codes.size());

    return std::make_pair(lit_tree, dst_tree);
}

// All multi-byte numbers in the format described here are stored with
// the least-significant byte first (at the lower memory address).
//
//  * Data elements are packed into bytes in order of
//    increasing bit number within the byte, i.e., starting
//    with the least-significant bit of the byte.
//  * Data elements other than Huffman codes are packed
//    starting with the least-significant bit of the data
//    element.
//  * Huffman codes are packed starting with the most-
//    significant bit of the code.
//
// THEREFORE:
//  1. Multi-byte numbers are little-endian
//  2. Huffman codes are are packed most significant -> least significant
//  3. Everything else is least significant -> most significant

struct BitWriter {
    using Buffer = uint32_t;

    BitWriter(FILE* fp) noexcept : out_{fp}, buff_{0}, bits_{0} {}

    void write_bits(uint16_t val, size_t n_bits) noexcept
    {
        // DEBUG("Enter write_bits: n_bits=%2zu bits_=%2zu buff_=0x%08x", n_bits, bits_, buff_);
        assert(n_bits <= MaxBits);
        auto room = BufferSizeInBits - bits_;
        if (room >= n_bits) {
            buff_ |= val << bits_;
            bits_ += n_bits;
        } else {
            auto n1 = std::min(room, n_bits);
            auto n2 = n_bits - n1;
            buff_ |= (val & _ones_mask(n1)) << bits_;
            bits_ += n1;
            write_full_buffer();
            assert(bits_ == 0);
            buff_ |= val >> n1;
            bits_ += n2;
        }
        assert(bits_ <= BufferSizeInBits);
        // DEBUG("Exit  write_bits: n_bits=%2zu bits_=%2zu buff_=0x%08x", n_bits, bits_, buff_);
    }

    void write(const void* p, size_t size) noexcept {
        flush();
        xwrite(p, 1, size, out_);
    }

    void flush() noexcept {
        auto n_bytes = (bits_ + 7) / 8;
        // DEBUG("flush: bits_=%zu buff_=0x%08x n_bytes=%zu", bits_, buff_, n_bytes);
        xwrite(&buff_, 1, n_bytes, out_);
        buff_ = 0;
        bits_ = 0;
    }

    void write_full_buffer() noexcept {
        assert(bits_ == BufferSizeInBits);
        // DEBUG("write_full_buffer: bits_=%zu buff_=0x%08x", bits_, buff_);
        xwrite(&buff_, sizeof(buff_), 1, out_);
        buff_ = 0;
        bits_ = 0;
    }

    static constexpr Buffer _ones_mask(size_t n_bits) noexcept {
        assert(n_bits <= BufferSizeInBits);
        if (n_bits == BufferSizeInBits) {
            return static_cast<Buffer>(-1);
        } else {
            return static_cast<Buffer>((1u << n_bits) - 1);
        }
    }

    Buffer buff_ = 0;
    size_t bits_ = 0;
    FILE*  out_ = nullptr;

    constexpr static size_t BufferSizeInBits = 32;
    static_assert((sizeof(Buffer) * CHAR_BIT) >= BufferSizeInBits);
};

void blkwrite_no_compression(const char* buffer, size_t size, uint8_t bfinal, BitWriter& out) {
    uint8_t block_type = static_cast<uint8_t>(BType::NO_COMPRESSION);
    uint8_t btype = static_cast<uint8_t>(BType::NO_COMPRESSION);
    uint16_t len = size;
    uint16_t nlen = len ^ 0xffffu;
    out.write_bits(bfinal, 1);
    out.write_bits(block_type, 2);
    out.flush();
    // TODO: technically need to force little endian
    out.write(&len, sizeof(len));
    out.write(&nlen, sizeof(nlen));
    out.write(&buffer[0], size);
}

void blkwrite_fixed(const char* buf, size_t size, uint8_t bfinal, BitWriter& out) {
    auto&& [lits, dsts] = init_fixed_huffman_data();
    uint8_t block_type = static_cast<uint8_t>(BType::FIXED_HUFFMAN);
    out.write_bits(bfinal, 1);
    out.write_bits(block_type, 2);
    for (const char *p = buf, *end = buf + size; p != end; ++p) {
        auto val = static_cast<uint16_t>(*reinterpret_cast<const uint8_t*>(p));
        auto it = lits.find(val);
        if (it == lits.end()) {
            panic("no code for literal: %c (%u)", val, val);
        }
        auto&& [code, n_bits] = it->second;
        out.write_bits(code, n_bits);
        // DEBUG("value(%c, %u) => code(0x%02x) len=%u, flipped=0x%02x", val, val, code, n_bits, flip_code(code, n_bits));
    }
    {
        auto it = lits.find(static_cast<uint16_t>(256));
        assert(it != lits.end());
        auto&& [code, n_bits] = it->second;
        out.write_bits(code, n_bits);
        // DEBUG("value(%c) => code(0x%02x) len=%u, flipped=0x%02x", 256, code, n_bits, flip_code(code, n_bits));
    }
}

CodeLengths make_header_code_lengths(const CodeLengths& codelens) {
    constexpr std::array<int, NumHeaderCodeLengths> order = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::array<int, NumHeaderCodeLengths> counts;
    std::fill(counts.begin(), counts.end(), 0);
    for (auto&& codelen : codelens) {
        counts[codelen]++;
    }
    CodeLengths result;
    for (auto codelen : order) {
        result.push_back(counts[codelen]);
    }
    return result;
}

struct DynamicHeader {
    std::vector<int> codes;
    std::vector<int> extra;
    Tree             tree;
};

DynamicHeader make_header_tree(const CodeLengths& codelens) {
    std::vector<int> codes;
    std::vector<int> extra;
    int buf = -1;
    int cnt = 0;
    for (auto codelen : codelens) {
        if (cnt == 0) {
            buf = codelen;
            cnt = 1;
        } else if (buf == codelen) {
            cnt++;
        } else if (cnt < 3) {
            codes.insert(codes.end(), cnt, buf);
            extra.insert(extra.end(), cnt, 0);
            buf = codelen;
            cnt = 1;
        } else if (buf == 0) {
            assert(cnt >= 3);
            assert(codelen != buf);
            if (cnt <= 10) {
                codes.push_back(17);
                extra.push_back(static_cast<int>(cnt));
            } else {
                assert(cnt <= 138);
                codes.push_back(18);
                extra.push_back(static_cast<int>(cnt));
            }
            buf = codelen;
            cnt = 1;
        } else {
            assert(cnt >= 3);
            assert(buf != 0);
            if (codes.empty() || codes.back() != buf) {
                codes.push_back(buf);
                extra.push_back(0);
                cnt--;
            }
            while (cnt >= 3) {
                int repeat_amount = std::min(6, cnt);
                codes.push_back(16);
                extra.push_back(repeat_amount);
                cnt -= repeat_amount;
            }
            codes.insert(codes.end(), cnt, buf);
            extra.insert(extra.end(), cnt, 0);
            buf = codelen;
            cnt = 1;
        }

        assert(codes.size() == extra.size());
        assert(buf == codelen);
        assert(cnt > 0);
    }

    // flush
    if (cnt < 3) {
        codes.insert(codes.end(), cnt, buf);
        extra.insert(extra.end(), cnt, 0);
    } else if (buf == 0) {
        if (cnt <= 10) {
            codes.push_back(17);
            extra.push_back(cnt);
        } else {
            codes.push_back(18);
            extra.push_back(cnt);
        }
    } else {
        assert(cnt >= 3);
        assert(buf != 0);
        if (codes.empty() || codes.back() != buf) {
            codes.push_back(buf);
            extra.push_back(0);
            cnt--;
        }
        while (cnt >= 3) {
            int repeat_amount = std::min(6, cnt);
            codes.push_back(16);
            extra.push_back(repeat_amount);
            cnt -= repeat_amount;
        }
        codes.insert(codes.end(), cnt, buf);
        extra.insert(extra.end(), cnt, 0);
    }

    auto header_tree = construct_huffman_tree(codes.data(), codes.size());

    // TEMP TEMP
    printf("--- HEADER ENCODING ---\n");
    for (auto&& [value, bits] : header_tree) {
        printf("%d: %d\n", value, bits);
    }
    printf("--- END HEADER ENCODING ---\n");

    CodeLengths header_codelens(NumHeaderCodeLengths, 0);
    assert(header_codelens.size() == NumHeaderCodeLengths);
    for (auto&& [value, bits] : header_tree) {
        assert(0 <= value && value < NumHeaderCodeLengths);
        assert(0 <= bits && bits < MaxBits);
        header_codelens[value] = static_cast<uint8_t>(bits);
    }
    auto tree = init_huffman_tree(header_codelens.data(), header_codelens.size());
    return { codes, extra, tree };
}

CodeLengths make_header_tree_length_data(const Tree& tree) {
    constexpr std::array<int, NumHeaderCodeLengths> order = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    CodeLengths results(order.size(), 0);
    for (size_t index = 0; index < order.size(); ++index) {
        auto value = order[index];
        auto it = tree.find(value);
        if (it != tree.end()) {
            auto bits = it->second.bits;
            assert(0 <= bits && bits < MaxHeaderCodeLength);
            results[index] = bits;
        }
    }
    while (results.size() > 4 && results.back() == 0) {
        results.pop_back();
    }
    assert(4 <= results.size() && results.size() <= 19);
    return results;
}


[[maybe_unused]] auto&& safelit = [](uint16_t x) -> char {
    if (x < 256) {
        if (x >= '!') {
            return static_cast<char>(x);
        } else {
            return x == ' ' ? x : '*';
        }
    } else {
        return '?';
    }
};

void blkwrite_dynamic(const char* buf, size_t size, uint8_t bfinal, BitWriter& out) {
    DEBUG("blkwrite_dynamic: bfinal=%s size=%zu", bfinal ? "TRUE" : "FALSE", size);
    auto&& [codelens, hlit, hdist] = make_code_lengths();
    auto lits = init_huffman_tree(&codelens[0], hlit);
    auto dsts = init_huffman_tree(&codelens[hlit], hdist);
    auto&& [hcodes, hextra, htree] = make_header_tree(codelens);
    auto htree_length_data = make_header_tree_length_data(htree);
    auto hclen = htree_length_data.size();
    xassert(257 <= hlit && hlit <= 286, "hlit = %zu", hlit);
    xassert(1 <= hdist && hdist <= 32, "hdist = %zu", hdist);
    xassert(4 <= hclen && hclen <= 19, "hclen = %zu", hclen);

    DEBUG("hlit = %zu", hlit);
    DEBUG("hdist = %zu", hdist);
    DEBUG("hclen = %zu", hclen);

    // TEMP TEMP
    for (auto&& [value, code] : htree) {
        DEBUG("value=%2u code=0x%02x codelen=%u", value, code.code, code.bits);
    }

    uint8_t block_type = static_cast<uint8_t>(BType::DYNAMIC_HUFFMAN);
    out.write_bits(bfinal, 1);
    out.write_bits(block_type, 2);
    out.write_bits(hlit - 257, 5);
    out.write_bits(hdist - 1, 5);
    out.write_bits(hclen - 4, 4);

    // header tree code lengths
    for (auto codelen : htree_length_data) {
        out.write_bits(codelen, 3);
    }

    // literal and distance code lengths
    for (size_t i = 0; i < hcodes.size(); ++i) {
        auto codelen = hcodes[i];
        auto it = htree.find(codelen);
        xassert(it != htree.end(), "no code for header lit value: %u", codelen);
        auto&& [code, bits] = it->second;
        out.write_bits(code, bits);
        switch (codelen) {
            case 16:
                xassert(3 <= hextra[i] && hextra[i] <= 6, "invalid hextra: %d", hextra[i]);
                out.write_bits(hextra[i] - 3, 2);
                break;
            case 17:
                xassert(3 <= hextra[i] && hextra[i] <= 10, "invalid hextra: %d", hextra[i]);
                out.write_bits(hextra[i] - 3, 3);
                break;
            case 18:
                xassert(11 <= hextra[i] && hextra[i] <= 138, "invalid hextra: %d", hextra[i]);
                out.write_bits(hextra[i] - 11, 7);
                break;
            default:
                break;
        }
    }

    // huffman data
    for (const char *p = buf, *end = buf + size; p != end; ++p) {
        auto val = static_cast<uint16_t>(*reinterpret_cast<const uint8_t*>(p));
        auto it = lits.find(val);
        xassert(it != lits.end(), "no code for %u (%c)", val, *p);
        auto&& [code, n_bits] = it->second;
        // DEBUG("Writing val=%u (%c) code=0x%02x bits=%u", val, safelit(val), code, n_bits);
        out.write_bits(code, n_bits);
    }

    // end block marker
    {
        uint16_t val = 256;
        auto it = lits.find(static_cast<uint16_t>(val));
        assert(it != lits.end());
        auto&& [code, n_bits] = it->second;
        // DEBUG("value=%u code=0x%02x len=%u", val, code, n_bits);
        // DEBUG("Writing val=%u (%c) code=0x%02x bits=%u", val, safelit(val), code, n_bits);
        out.write_bits(code, n_bits);
    }
}



int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: %s [FILE]\n", argv[0]);
        exit(0);
    }
    std::string input_filename = argv[1];
    std::string output_filename = argc == 3 ? argv[2] : input_filename + ".gz";

    printf("Input Filename : %s\n", input_filename.c_str());
    printf("Output Filename: %s\n", output_filename.c_str());

    FileHandle fp = fopen(input_filename.c_str(), "rb");
    if (!fp) {
        perror("fopen");
        exit(1);
    }

    FileHandle out = fopen(output_filename.c_str(), "wb");
    if (!out) {
        perror("fopen");
        exit(1);
    }
    BitWriter writer{out};

    init_crc_table();

    // +---+---+---+---+---+---+---+---+---+---+
    // |ID1|ID2|CM |FLG|     MTIME     |XFL|OS | (more-->)
    // +---+---+---+---+---+---+---+---+---+---+

    uint8_t flags = static_cast<uint8_t>(Flags::FNAME);
    uint32_t mtime = 0;  // TODO: set mtime to seconds since epoch
    uint8_t xfl = 0;
    uint8_t os = 3;                                   // UNIX
    xwrite(&ID1_GZIP, sizeof(ID1_GZIP), 1, out);      // ID1
    xwrite(&ID2_GZIP, sizeof(ID2_GZIP), 1, out);      // ID2
    xwrite(&CM_DEFLATE, sizeof(CM_DEFLATE), 1, out);  // CM
    xwrite(&flags, sizeof(flags), 1, out);            // FLG
    xwrite(&mtime, sizeof(mtime), 1, out);            // MTIME
    xwrite(&xfl, sizeof(xfl), 1, out);                // XFL
    xwrite(&os, sizeof(os), 1, out);                  // OS

    //   +=========================================+
    //   |...original file name, zero-terminated...| (more-->)
    //   +=========================================+
    xwrite(input_filename.c_str(), input_filename.size() + 1, 1, out);  // FNAME

    uint32_t crc = calc_crc32(0, NULL, 0);
    // This contains the size of the original (uncompressed) input
    // data modulo 2^32.
    uint32_t isize = 0;

#if 0
#elif 0
    auto* compress_fn = &blkwrite_no_compression;
#elif 0
    auto* compress_fn = &blkwrite_fixed;
#elif 1
    auto* compress_fn = &blkwrite_dynamic;
#else
    #error "Must select an implementation"
#endif

    char buf[BUFSIZE];
    size_t size = 0;
    size_t read;
    while ((read = fread(&buf[size], 1, READSIZE, fp)) > 0) {
        crc = calc_crc32(crc, &buf[size], read);
        isize += read;
        size += read;
        if (size > BLOCKSIZE) {
            DEBUG("Calling compress on block of size: %zu", size);
            compress_fn(buf, BLOCKSIZE, 0, writer);
            size -= BLOCKSIZE;
            memmove(&buf[0], &buf[BLOCKSIZE], size);
        }
    }
    if (ferror(fp)) {
        panic("error reading from file");
    }
    assert(feof(fp));

    // If the input file is empty, do need to write at least 1 block, which can
    // contain no data.
    //
    // NOTE: it is compliant to have unnecessary empty blocks so in the very
    // rare case that `size` == 0, and isize wraps to exactly 0, will write
    // an unnecessary empty block, but that is OK.
    if (size > 0 || isize == 0) {
        DEBUG("Flushing final block size: %zu", size);
        compress_fn(buf, size, 1, writer);
    }
    writer.flush();

    DEBUG("CRC32 = 0x%08x", crc);
    DEBUG("ISIZE = 0x%08x", isize);

    //   0   1   2   3   4   5   6   7
    // +---+---+---+---+---+---+---+---+
    // |     CRC32     |     ISIZE     |
    // +---+---+---+---+---+---+---+---+
    xwrite(&crc, sizeof(crc), 1, out);      // CRC32
    xwrite(&isize, sizeof(isize), 1, out);  // ISIZE

    return 0;
}
