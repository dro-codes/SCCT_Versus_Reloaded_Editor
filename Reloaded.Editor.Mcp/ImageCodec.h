#pragma once
// Images without Windows or zlib: PNG, BMP and TGA decoded to 8-bit RGBA,
// PNG, BMP and TGA encoded, and resizing. The exe uses it for plan renders.
// Engine-free and Windows-free.
//
// PNG: every colour type (grey, RGB, palette, grey+alpha, RGBA) at the bit
// depths the format allows (1/2/4/8 for grey and palette, 8 and 16 for the
// rest; 16-bit samples keep their high byte), tRNS transparency, all five
// filters. Interlaced (Adam7) files are refused with a clear error.
// BMP: 1/4/8-bit palettes, 16-bit 555/565, 24 and 32-bit, BI_RGB and
// BI_BITFIELDS, bottom-up or top-down; RLE compression is refused.
// TGA: true colour (16/24/32), grey and colour-mapped, raw or RLE, either origin.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace Mcp::Image
{
    // The largest image any decoder accepts: 16384 a side and 64 megapixels
    // (256 MB of RGBA), so a corrupt header cannot exhaust the 32-bit exe.
    constexpr int kMaxEdge = 16384;
    constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;

    // 8-bit RGBA, rows top to bottom, no padding.
    struct Rgba
    {
        int width = 0, height = 0;
        std::vector<uint8_t> pixels;
        Rgba() = default;
        Rgba(int w, int h, std::array<uint8_t, 4> fill = {0, 0, 0, 255}) : width(w), height(h)
        {
            if (w < 0 || h < 0 || w > kMaxEdge || h > kMaxEdge || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kMaxPixels)
                throw std::runtime_error("Image size " + std::to_string(w) + "x" + std::to_string(h) + " is outside the supported range.");
            pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            for (size_t i = 0; i < pixels.size(); i += 4) std::memcpy(&pixels[i], fill.data(), 4);
        }
        uint8_t* At(int x, int y) { return &pixels[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4]; }
        const uint8_t* At(int x, int y) const { return &pixels[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4]; }
        bool Empty() const { return width == 0 || height == 0; }
        bool Opaque() const
        {
            for (size_t i = 3; i < pixels.size(); i += 4) if (pixels[i] != 255) return false;
            return true;
        }
    };

    // --- Checksums -----------------------------------------------------------
    inline uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0)
    {
        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> t{};
            for (uint32_t n = 0; n < 256; ++n)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[n] = c;
            }
            return t;
        }();
        const auto* p = static_cast<const uint8_t*>(data);
        crc = ~crc;
        for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
        return ~crc;
    }
    inline uint32_t Adler32(const void* data, size_t size, uint32_t adler = 1)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        uint32_t a = adler & 0xffff, b = adler >> 16;
        while (size)
        {
            // 5552 bytes is the most that cannot overflow before the modulo.
            const size_t run = std::min<size_t>(size, 5552);
            for (size_t i = 0; i < run; ++i) { a += p[i]; b += a; }
            a %= 65521; b %= 65521;
            p += run; size -= run;
        }
        return (b << 16) | a;
    }

    // --- Inflate (RFC 1951), and the zlib and gzip wrappers -------------------
    namespace Detail
    {
        inline uint32_t Be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3]; }
        inline uint32_t Le32(const uint8_t* p) { return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[0]; }
        inline uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        inline void PutBe32(std::string& out, uint32_t v) { for (int s = 24; s >= 0; s -= 8) out += static_cast<char>((v >> s) & 0xff); }
        inline void PutLe32(std::string& out, uint32_t v) { for (int s = 0; s < 32; s += 8) out += static_cast<char>((v >> s) & 0xff); }
        inline void PutLe16(std::string& out, uint32_t v) { out += static_cast<char>(v & 0xff); out += static_cast<char>((v >> 8) & 0xff); }
        inline const uint8_t* Bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

        constexpr uint16_t kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        struct BitReader
        {
            const uint8_t* data = nullptr;
            size_t size = 0, pos = 0;
            uint32_t buffer = 0;
            int count = 0;
            uint32_t Bits(int n)
            {
                while (count < n)
                {
                    if (pos >= size) throw std::runtime_error("The compressed data ends early (truncated or corrupt file).");
                    buffer |= static_cast<uint32_t>(data[pos++]) << count;
                    count += 8;
                }
                const uint32_t value = n == 0 ? 0 : buffer & ((1u << n) - 1);
                buffer = n == 32 ? 0 : buffer >> n;
                count -= n;
                return value;
            }
            void Align() { buffer = 0; count = 0; }
        };
        // Canonical Huffman code: how many codes of each length, and the
        // symbols in code order (as zlib's puff decodes them).
        struct Huffman
        {
            std::array<uint16_t, 16> counts{};
            std::vector<uint16_t> symbols;
        };
        inline void Build(Huffman& h, const uint8_t* lengths, int n)
        {
            h.counts.fill(0);
            for (int s = 0; s < n; ++s) h.counts[lengths[s]]++;
            int left = 1;
            for (int len = 1; len < 16; ++len)
            {
                left <<= 1;
                left -= h.counts[static_cast<size_t>(len)];
                if (left < 0) throw std::runtime_error("The compressed data has an invalid Huffman table.");
            }
            std::array<uint16_t, 16> offsets{};
            for (int len = 1; len < 15; ++len) offsets[static_cast<size_t>(len) + 1] = static_cast<uint16_t>(offsets[static_cast<size_t>(len)] + h.counts[static_cast<size_t>(len)]);
            h.symbols.assign(static_cast<size_t>(n), 0);
            for (int s = 0; s < n; ++s)
                if (lengths[s]) h.symbols[offsets[lengths[s]]++] = static_cast<uint16_t>(s);
        }
        inline int Decode(BitReader& in, const Huffman& h)
        {
            int code = 0, first = 0, index = 0;
            for (int len = 1; len < 16; ++len)
            {
                code |= static_cast<int>(in.Bits(1));
                const int count = h.counts[static_cast<size_t>(len)];
                if (code - count < first) return h.symbols[static_cast<size_t>(index + (code - first))];
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            throw std::runtime_error("The compressed data has an invalid Huffman code.");
        }
        inline void Codes(BitReader& in, std::string& out, const Huffman& lit, const Huffman& dist, size_t limit)
        {
            for (;;)
            {
                const int symbol = Decode(in, lit);
                if (symbol < 256)
                {
                    if (out.size() >= limit) throw std::runtime_error("The compressed data is larger than expected.");
                    out += static_cast<char>(symbol);
                    continue;
                }
                if (symbol == 256) return;
                const int k = symbol - 257;
                if (k >= 29) throw std::runtime_error("The compressed data has an invalid length code.");
                const size_t length = kLengthBase[k] + in.Bits(kLengthExtra[k]);
                const int d = Decode(in, dist);
                if (d >= 30) throw std::runtime_error("The compressed data has an invalid distance code.");
                const size_t distance = kDistBase[d] + in.Bits(kDistExtra[d]);
                if (distance > out.size()) throw std::runtime_error("The compressed data refers back past its start.");
                if (out.size() + length > limit) throw std::runtime_error("The compressed data is larger than expected.");
                const size_t from = out.size() - distance;
                for (size_t i = 0; i < length; ++i) out += out[from + i];
            }
        }
    }
    // Raw deflate data to bytes, at most limit of them. used (optional)
    // receives how many input bytes the stream took.
    inline std::string Inflate(const std::string& data, size_t limit = 256u * 1024 * 1024, size_t* used = nullptr)
    {
        using namespace Detail;
        BitReader in;
        in.data = Bytes(data);
        in.size = data.size();
        std::string out;
        static const std::pair<Huffman, Huffman> fixed = []
        {
            std::array<uint8_t, 288> lengths{};
            for (int s = 0; s < 288; ++s) lengths[static_cast<size_t>(s)] = static_cast<uint8_t>(s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8);
            std::pair<Huffman, Huffman> codes;
            Build(codes.first, lengths.data(), 288);
            std::array<uint8_t, 30> distances{};
            distances.fill(5);
            Build(codes.second, distances.data(), 30);
            return codes;
        }();
        for (bool last = false; !last;)
        {
            last = in.Bits(1) != 0;
            const uint32_t type = in.Bits(2);
            if (type == 0)
            {
                in.Align();
                if (in.pos + 4 > in.size) throw std::runtime_error("The compressed data ends early (truncated or corrupt file).");
                const uint16_t length = Le16(in.data + in.pos), check = Le16(in.data + in.pos + 2);
                if (static_cast<uint16_t>(~check) != length) throw std::runtime_error("The compressed data has a corrupt stored block.");
                in.pos += 4;
                if (in.pos + length > in.size) throw std::runtime_error("The compressed data ends early (truncated or corrupt file).");
                if (out.size() + length > limit) throw std::runtime_error("The compressed data is larger than expected.");
                out.append(reinterpret_cast<const char*>(in.data + in.pos), length);
                in.pos += length;
            }
            else if (type == 1) Codes(in, out, fixed.first, fixed.second, limit);
            else if (type == 2)
            {
                const int nlen = static_cast<int>(in.Bits(5)) + 257, ndist = static_cast<int>(in.Bits(5)) + 1, ncode = static_cast<int>(in.Bits(4)) + 4;
                if (nlen > 286 || ndist > 30) throw std::runtime_error("The compressed data has an invalid block header.");
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::array<uint8_t, 320> lengths{};
                for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(in.Bits(3));
                Huffman lencode;
                Build(lencode, lengths.data(), 19);
                lengths.fill(0);
                for (int index = 0; index < nlen + ndist;)
                {
                    const int symbol = Decode(in, lencode);
                    if (symbol < 16) { lengths[static_cast<size_t>(index++)] = static_cast<uint8_t>(symbol); continue; }
                    uint8_t value = 0;
                    int repeat = 0;
                    if (symbol == 16)
                    {
                        if (index == 0) throw std::runtime_error("The compressed data repeats a length before the first.");
                        value = lengths[static_cast<size_t>(index - 1)];
                        repeat = 3 + static_cast<int>(in.Bits(2));
                    }
                    else if (symbol == 17) repeat = 3 + static_cast<int>(in.Bits(3));
                    else repeat = 11 + static_cast<int>(in.Bits(7));
                    if (index + repeat > nlen + ndist) throw std::runtime_error("The compressed data has too many code lengths.");
                    while (repeat--) lengths[static_cast<size_t>(index++)] = value;
                }
                if (!lengths[256]) throw std::runtime_error("The compressed data has no end-of-block code.");
                Huffman lit, dist;
                Build(lit, lengths.data(), nlen);
                Build(dist, lengths.data() + nlen, ndist);
                Codes(in, out, lit, dist, limit);
            }
            else throw std::runtime_error("The compressed data has an invalid block type.");
        }
        if (used) *used = in.pos;
        return out;
    }
    inline std::string ZlibDecompress(const std::string& data, size_t limit = 256u * 1024 * 1024)
    {
        const auto* p = Detail::Bytes(data);
        if (data.size() < 6 || (p[0] & 0x0f) != 8 || ((p[0] << 8) | p[1]) % 31 != 0) throw std::runtime_error("The data is not a zlib stream.");
        if (p[1] & 0x20) throw std::runtime_error("The zlib stream needs a preset dictionary, which is not supported.");
        size_t used = 0;
        auto out = Inflate(data.substr(2), limit, &used);
        if (2 + used + 4 <= data.size() && Detail::Be32(p + 2 + used) != Adler32(out.data(), out.size()))
            throw std::runtime_error("The zlib stream's checksum does not match (corrupt data).");
        return out;
    }
    inline std::string GzipDecompress(const std::string& data, size_t limit = 256u * 1024 * 1024)
    {
        const auto* p = Detail::Bytes(data);
        if (data.size() < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) throw std::runtime_error("The data is not a gzip stream.");
        const int flags = p[3];
        size_t at = 10;
        if (flags & 4) { if (at + 2 > data.size()) throw std::runtime_error("The gzip header is truncated."); at += 2 + Detail::Le16(p + at); }
        for (int bit : {8, 16})
            if (flags & bit) { while (at < data.size() && p[at]) ++at; ++at; }
        if (flags & 2) at += 2;
        if (at >= data.size()) throw std::runtime_error("The gzip header is truncated.");
        size_t used = 0;
        auto out = Inflate(data.substr(at), limit, &used);
        if (at + used + 8 <= data.size() && Detail::Le32(p + at + used) != Crc32(out.data(), out.size()))
            throw std::runtime_error("The gzip stream's checksum does not match (corrupt data).");
        return out;
    }

    // --- Deflate -------------------------------------------------------------
    namespace Detail
    {
        struct BitWriter
        {
            std::string out;
            uint32_t buffer = 0;
            int count = 0;
            void Put(uint32_t value, int n)
            {
                buffer |= value << count;
                count += n;
                while (count >= 8) { out += static_cast<char>(buffer & 0xff); buffer >>= 8; count -= 8; }
            }
            // Huffman codes go most significant bit first.
            void Code(uint32_t code, int n)
            {
                uint32_t reversed = 0;
                for (int i = 0; i < n; ++i) reversed |= ((code >> i) & 1u) << (n - 1 - i);
                Put(reversed, n);
            }
            void Flush() { if (count > 0) { out += static_cast<char>(buffer & 0xff); buffer = 0; count = 0; } }
        };
        inline void FixedLiteral(BitWriter& w, int symbol)
        {
            if (symbol < 144) w.Code(static_cast<uint32_t>(0x30 + symbol), 8);
            else if (symbol < 256) w.Code(static_cast<uint32_t>(0x190 + symbol - 144), 9);
            else if (symbol < 280) w.Code(static_cast<uint32_t>(symbol - 256), 7);
            else w.Code(static_cast<uint32_t>(0xC0 + symbol - 280), 8);
        }
    }
    // Raw deflate. Level 0 writes stored blocks; any other level a single
    // fixed-Huffman block with LZ77 matches (hash chains, greedy), which is
    // what flat-coloured plans and masks need: they shrink a hundredfold.
    inline std::string Deflate(const std::string& data, int level = 6)
    {
        using namespace Detail;
        BitWriter w;
        if (level <= 0)
        {
            size_t at = 0;
            do
            {
                const size_t run = std::min<size_t>(data.size() - at, 65535);
                const bool last = at + run >= data.size();
                w.Put(last ? 1u : 0u, 1);
                w.Put(0, 2);
                w.Flush();
                PutLe16(w.out, static_cast<uint32_t>(run));
                PutLe16(w.out, static_cast<uint32_t>(~run & 0xffff));
                w.out.append(data, at, run);
                at += run;
            } while (at < data.size());
            return w.out;
        }
        w.Put(1, 1);
        w.Put(1, 2);
        const auto* p = Bytes(data);
        const size_t n = data.size();
        constexpr size_t kWindow = 32768, kHashSize = 1 << 15;
        const int maxChain = std::clamp(level * 16, 8, 256);
        std::vector<int32_t> head(kHashSize, -1), prev(kWindow, -1);
        auto hash = [&](size_t i) { return ((static_cast<uint32_t>(p[i]) << 10) ^ (static_cast<uint32_t>(p[i + 1]) << 5) ^ p[i + 2]) & (kHashSize - 1); };
        auto insert = [&](size_t i)
        {
            if (i + 2 >= n) return;
            const auto h = hash(i);
            prev[i % kWindow] = head[h];
            head[h] = static_cast<int32_t>(i);
        };
        for (size_t i = 0; i < n;)
        {
            size_t bestLength = 0, bestDistance = 0;
            if (i + 2 < n)
            {
                int32_t candidate = head[hash(i)];
                const size_t maxLength = std::min<size_t>(258, n - i);
                for (int chain = 0; candidate >= 0 && chain < maxChain; ++chain)
                {
                    const size_t c = static_cast<size_t>(candidate);
                    if (i - c > kWindow - 1) break;
                    if (p[c + bestLength] == p[i + bestLength] || bestLength == 0)
                    {
                        size_t length = 0;
                        while (length < maxLength && p[c + length] == p[i + length]) ++length;
                        if (length > bestLength) { bestLength = length; bestDistance = i - c; if (length == maxLength) break; }
                    }
                    const int32_t next = prev[c % kWindow];
                    if (next >= candidate) break;
                    candidate = next;
                }
            }
            if (bestLength >= 3)
            {
                int k = 28;
                while (kLengthBase[k] > bestLength) --k;
                FixedLiteral(w, 257 + k);
                w.Put(static_cast<uint32_t>(bestLength - kLengthBase[k]), kLengthExtra[k]);
                int d = 29;
                while (kDistBase[d] > bestDistance) --d;
                w.Code(static_cast<uint32_t>(d), 5);
                w.Put(static_cast<uint32_t>(bestDistance - kDistBase[d]), kDistExtra[d]);
                for (size_t k2 = 0; k2 < bestLength; ++k2) insert(i + k2);
                i += bestLength;
            }
            else
            {
                FixedLiteral(w, p[i]);
                insert(i);
                ++i;
            }
        }
        FixedLiteral(w, 256);
        w.Flush();
        return w.out;
    }
    inline std::string ZlibCompress(const std::string& data, int level = 6)
    {
        std::string out;
        out += static_cast<char>(0x78);
        out += static_cast<char>(level <= 0 ? 0x01 : 0x9C);
        out += Deflate(data, level);
        Detail::PutBe32(out, Adler32(data.data(), data.size()));
        return out;
    }

    // --- PNG -----------------------------------------------------------------
    namespace Detail
    {
        inline uint8_t Paeth(int a, int b, int c)
        {
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
        }
        inline void Unfilter(uint8_t filter, uint8_t* row, const uint8_t* above, size_t length, size_t bpp)
        {
            switch (filter)
            {
            case 0: return;
            case 1: for (size_t i = bpp; i < length; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - bpp]); return;
            case 2: if (above) for (size_t i = 0; i < length; ++i) row[i] = static_cast<uint8_t>(row[i] + above[i]); return;
            case 3:
                for (size_t i = 0; i < length; ++i)
                {
                    const int left = i >= bpp ? row[i - bpp] : 0, up = above ? above[i] : 0;
                    row[i] = static_cast<uint8_t>(row[i] + ((left + up) >> 1));
                }
                return;
            case 4:
                for (size_t i = 0; i < length; ++i)
                {
                    const int left = i >= bpp ? row[i - bpp] : 0, up = above ? above[i] : 0, corner = (above && i >= bpp) ? above[i - bpp] : 0;
                    row[i] = static_cast<uint8_t>(row[i] + Paeth(left, up, corner));
                }
                return;
            default: throw std::runtime_error("The PNG has an invalid row filter (corrupt file).");
            }
        }
        inline void Chunk(std::string& out, const char* type, const std::string& data)
        {
            PutBe32(out, static_cast<uint32_t>(data.size()));
            const std::string body = std::string(type, 4) + data;
            out += body;
            PutBe32(out, Crc32(body.data(), body.size()));
        }
    }
    inline bool IsPng(const std::string& bytes) { return bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0; }
    // The size a PNG says it has, without decoding it.
    inline std::pair<int, int> PngSize(const std::string& bytes)
    {
        if (!IsPng(bytes) || bytes.size() < 24 || bytes.compare(12, 4, "IHDR") != 0) throw std::runtime_error("Not a PNG file.");
        const auto* p = Detail::Bytes(bytes);
        return {static_cast<int>(Detail::Be32(p + 16)), static_cast<int>(Detail::Be32(p + 20))};
    }
    inline Rgba DecodePng(const std::string& bytes)
    {
        using namespace Detail;
        if (!IsPng(bytes)) throw std::runtime_error("Not a PNG file (the signature is missing).");
        const auto* p = Bytes(bytes);
        size_t at = 8;
        uint32_t width = 0, height = 0;
        int depth = 0, colour = -1;
        std::vector<std::array<uint8_t, 4>> palette;
        std::string idat, trns;
        bool header = false, ended = false;
        while (at + 12 <= bytes.size() && !ended)
        {
            const uint32_t length = Be32(p + at);
            if (length > bytes.size() - at - 12) throw std::runtime_error("The PNG is truncated (a chunk runs past the end of the file).");
            const std::string type(reinterpret_cast<const char*>(p + at + 4), 4);
            const auto* data = p + at + 8;
            if (Be32(data + length) != Crc32(p + at + 4, length + 4)) throw std::runtime_error("The PNG's " + type + " chunk is corrupt (checksum mismatch).");
            if (type == "IHDR")
            {
                if (length != 13) throw std::runtime_error("The PNG header is malformed.");
                width = Be32(data);
                height = Be32(data + 4);
                depth = data[8];
                colour = data[9];
                if (data[10] != 0 || data[11] != 0) throw std::runtime_error("The PNG uses an unknown compression or filter method.");
                if (data[12] == 1) throw std::runtime_error("Interlaced (Adam7) PNGs are not supported; save the image without interlacing and try again.");
                if (data[12] != 0) throw std::runtime_error("The PNG uses an unknown interlace method.");
                header = true;
            }
            else if (type == "PLTE")
            {
                if (length % 3 || length > 768) throw std::runtime_error("The PNG palette is malformed.");
                for (uint32_t i = 0; i < length; i += 3) palette.push_back({data[i], data[i + 1], data[i + 2], 255});
            }
            else if (type == "tRNS") trns.assign(reinterpret_cast<const char*>(data), length);
            else if (type == "IDAT") idat.append(reinterpret_cast<const char*>(data), length);
            else if (type == "IEND") ended = true;
            else if (!(type[0] & 0x20)) throw std::runtime_error("The PNG has a critical chunk this reader does not know (" + type + ").");
            at += 12 + length;
        }
        if (!header) throw std::runtime_error("The PNG has no header chunk.");
        if (width == 0 || height == 0 || width > static_cast<uint32_t>(kMaxEdge) || height > static_cast<uint32_t>(kMaxEdge) || static_cast<uint64_t>(width) * height > kMaxPixels)
            throw std::runtime_error("The PNG is " + std::to_string(width) + "x" + std::to_string(height) + "; images up to " + std::to_string(kMaxEdge) + " a side and 64 megapixels are supported.");
        int channels = 0;
        bool ok = false;
        switch (colour)
        {
        case 0: channels = 1; ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
        case 2: channels = 3; ok = depth == 8 || depth == 16; break;
        case 3: channels = 1; ok = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
        case 4: channels = 2; ok = depth == 8 || depth == 16; break;
        case 6: channels = 4; ok = depth == 8 || depth == 16; break;
        default: break;
        }
        if (!ok) throw std::runtime_error("The PNG's colour type " + std::to_string(colour) + " at bit depth " + std::to_string(depth) + " is not valid.");
        if (colour == 3 && palette.empty()) throw std::runtime_error("The palette PNG has no palette.");
        const size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(depth);
        const size_t rowBytes = (static_cast<size_t>(width) * bitsPerPixel + 7) / 8, bpp = std::max<size_t>(1, bitsPerPixel / 8);
        const size_t expected = static_cast<size_t>(height) * (rowBytes + 1);
        auto raw = ZlibDecompress(idat, expected);
        if (raw.size() < expected) throw std::runtime_error("The PNG's image data is truncated.");
        auto* rows = reinterpret_cast<uint8_t*>(raw.data());
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t* row = rows + static_cast<size_t>(y) * (rowBytes + 1);
            Unfilter(row[0], row + 1, y ? rows + static_cast<size_t>(y - 1) * (rowBytes + 1) + 1 : nullptr, rowBytes, bpp);
        }
        if (colour == 3)
            for (size_t i = 0; i < trns.size() && i < palette.size(); ++i) palette[i][3] = static_cast<uint8_t>(trns[i]);
        Rgba image(static_cast<int>(width), static_cast<int>(height));
        const int maxSample = (1 << depth) - 1;
        // The raw sample (before scaling to 8 bits), for tRNS colour keys.
        auto sample = [&](const uint8_t* row, uint32_t x, int c) -> int
        {
            if (depth == 16) { const size_t at2 = (static_cast<size_t>(x) * static_cast<size_t>(channels) + static_cast<size_t>(c)) * 2; return (row[at2] << 8) | row[at2 + 1]; }
            if (depth == 8) return row[static_cast<size_t>(x) * static_cast<size_t>(channels) + static_cast<size_t>(c)];
            const size_t bit = static_cast<size_t>(x) * static_cast<size_t>(depth);
            return (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & maxSample;
        };
        auto to8 = [&](int v) { return static_cast<uint8_t>(depth == 16 ? v >> 8 : depth == 8 ? v : v * 255 / maxSample); };
        int keyGrey = -1, keyR = -1, keyG = -1, keyB = -1;
        if (colour == 0 && trns.size() >= 2) keyGrey = (static_cast<uint8_t>(trns[0]) << 8) | static_cast<uint8_t>(trns[1]);
        if (colour == 2 && trns.size() >= 6)
        {
            keyR = (static_cast<uint8_t>(trns[0]) << 8) | static_cast<uint8_t>(trns[1]);
            keyG = (static_cast<uint8_t>(trns[2]) << 8) | static_cast<uint8_t>(trns[3]);
            keyB = (static_cast<uint8_t>(trns[4]) << 8) | static_cast<uint8_t>(trns[5]);
        }
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t* row = rows + static_cast<size_t>(y) * (rowBytes + 1) + 1;
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t* out = image.At(static_cast<int>(x), static_cast<int>(y));
                switch (colour)
                {
                case 0: { const int g = sample(row, x, 0); out[0] = out[1] = out[2] = to8(g); out[3] = g == keyGrey ? 0 : 255; break; }
                case 2:
                {
                    const int r = sample(row, x, 0), g = sample(row, x, 1), b = sample(row, x, 2);
                    out[0] = to8(r); out[1] = to8(g); out[2] = to8(b);
                    out[3] = (r == keyR && g == keyG && b == keyB) ? 0 : 255;
                    break;
                }
                case 3:
                {
                    const int index = sample(row, x, 0);
                    if (static_cast<size_t>(index) >= palette.size()) throw std::runtime_error("The PNG uses a colour its palette does not have.");
                    std::memcpy(out, palette[static_cast<size_t>(index)].data(), 4);
                    break;
                }
                case 4: out[0] = out[1] = out[2] = to8(sample(row, x, 0)); out[3] = to8(sample(row, x, 1)); break;
                default: for (int c = 0; c < 4; ++c) out[c] = to8(sample(row, x, c)); break;
                }
            }
        }
        return image;
    }
    struct PngOptions
    {
        int level = 6;        // 0 stores the data uncompressed
        bool palette = true;  // write a palette image when 256 colours or fewer
        bool alpha = true;    // keep transparency (false writes RGB, alpha dropped)
    };
    inline std::string EncodePng(const Rgba& image, const PngOptions& options = {})
    {
        using namespace Detail;
        if (image.Empty()) throw std::runtime_error("An empty image cannot be written as a PNG.");
        if (image.pixels.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4) throw std::runtime_error("The image's pixel buffer does not match its size.");
        const bool keepAlpha = options.alpha && !image.Opaque();
        // A palette when the image has few colours: plans and masks do.
        std::vector<uint32_t> colours;
        std::unordered_map<uint32_t, uint8_t> index;
        bool usePalette = options.palette;
        if (usePalette)
        {
            for (size_t i = 0; i < image.pixels.size() && usePalette; i += 4)
            {
                const uint32_t key = (static_cast<uint32_t>(image.pixels[i]) << 24) | (static_cast<uint32_t>(image.pixels[i + 1]) << 16) |
                                     (static_cast<uint32_t>(image.pixels[i + 2]) << 8) | (keepAlpha ? image.pixels[i + 3] : 255u);
                if (index.count(key)) continue;
                if (colours.size() == 256) { usePalette = false; break; }
                index.emplace(key, static_cast<uint8_t>(colours.size()));
                colours.push_back(key);
            }
        }
        const int colourType = usePalette ? 3 : keepAlpha ? 6 : 2;
        const size_t channels = usePalette ? 1 : keepAlpha ? 4 : 3;
        const size_t rowBytes = static_cast<size_t>(image.width) * channels;
        std::string filtered;
        filtered.reserve(static_cast<size_t>(image.height) * (rowBytes + 1));
        std::vector<uint8_t> row(rowBytes), above(rowBytes, 0), trial(rowBytes), best(rowBytes);
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const uint8_t* px = image.At(x, y);
                const size_t at = static_cast<size_t>(x) * channels;
                if (usePalette)
                {
                    const uint32_t key = (static_cast<uint32_t>(px[0]) << 24) | (static_cast<uint32_t>(px[1]) << 16) | (static_cast<uint32_t>(px[2]) << 8) | (keepAlpha ? px[3] : 255u);
                    row[at] = index.at(key);
                }
                else for (size_t c = 0; c < channels; ++c) row[at + c] = px[c];
            }
            // Palette rows compress best unfiltered; others take the filter
            // with the smallest sum of signed residuals (the usual heuristic).
            uint8_t chosen = 0;
            best = row;
            if (!usePalette)
            {
                long bestScore = -1;
                for (uint8_t filter = 0; filter < 5; ++filter)
                {
                    long score = 0;
                    for (size_t i = 0; i < rowBytes; ++i)
                    {
                        const int left = i >= channels ? row[i - channels] : 0, up = y ? above[i] : 0, corner = (y && i >= channels) ? above[i - channels] : 0;
                        int predicted = 0;
                        if (filter == 1) predicted = left;
                        else if (filter == 2) predicted = up;
                        else if (filter == 3) predicted = (left + up) >> 1;
                        else if (filter == 4) predicted = Paeth(left, up, corner);
                        trial[i] = static_cast<uint8_t>(row[i] - predicted);
                        score += std::abs(static_cast<int8_t>(trial[i]));
                    }
                    if (bestScore < 0 || score < bestScore) { bestScore = score; chosen = filter; best = trial; }
                }
            }
            filtered += static_cast<char>(chosen);
            filtered.append(reinterpret_cast<const char*>(best.data()), rowBytes);
            above = row;
        }
        std::string out("\x89PNG\r\n\x1a\n", 8);
        std::string header;
        PutBe32(header, static_cast<uint32_t>(image.width));
        PutBe32(header, static_cast<uint32_t>(image.height));
        header += static_cast<char>(8);
        header += static_cast<char>(colourType);
        header.append(3, '\0');
        Chunk(out, "IHDR", header);
        if (usePalette)
        {
            std::string plte, trns;
            bool anyAlpha = false;
            for (auto key : colours)
            {
                plte += static_cast<char>(key >> 24);
                plte += static_cast<char>((key >> 16) & 0xff);
                plte += static_cast<char>((key >> 8) & 0xff);
                trns += static_cast<char>(key & 0xff);
                anyAlpha = anyAlpha || (key & 0xff) != 0xff;
            }
            Chunk(out, "PLTE", plte);
            if (anyAlpha) Chunk(out, "tRNS", trns);
        }
        Chunk(out, "IDAT", ZlibCompress(filtered, options.level));
        Chunk(out, "IEND", std::string());
        return out;
    }

    // --- BMP -----------------------------------------------------------------
    inline Rgba DecodeBmp(const std::string& bytes)
    {
        using namespace Detail;
        const auto* p = Bytes(bytes);
        if (bytes.size() < 26 || p[0] != 'B' || p[1] != 'M') throw std::runtime_error("Not a BMP file.");
        const uint32_t offset = Le32(p + 10), headerSize = Le32(p + 14);
        if (headerSize < 12 || 14 + static_cast<size_t>(headerSize) > bytes.size()) throw std::runtime_error("The BMP header is truncated.");
        int32_t width = 0, height = 0;
        int bits = 0;
        uint32_t compression = 0, colours = 0;
        if (headerSize == 12)
        {
            width = Le16(p + 18);
            height = static_cast<int16_t>(Le16(p + 20));
            bits = Le16(p + 24);
        }
        else
        {
            width = static_cast<int32_t>(Le32(p + 18));
            height = static_cast<int32_t>(Le32(p + 22));
            bits = Le16(p + 28);
            compression = Le32(p + 30);
            colours = Le32(p + 46);
        }
        const bool topDown = height < 0;
        if (height < 0) height = -height;
        if (width <= 0 || height <= 0 || width > kMaxEdge || height > kMaxEdge || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > kMaxPixels)
            throw std::runtime_error("The BMP's size " + std::to_string(width) + "x" + std::to_string(height) + " is outside the supported range.");
        if (compression == 1 || compression == 2) throw std::runtime_error("RLE-compressed BMPs are not supported; save it uncompressed (24-bit) and try again.");
        if (compression != 0 && compression != 3 && compression != 6) throw std::runtime_error("This BMP's compression (" + std::to_string(compression) + ") is not supported; save it as an uncompressed 24-bit BMP.");
        // Channel masks: BI_BITFIELDS after the 40-byte header (or inside a V4/V5 header).
        uint32_t masks[4] = {0, 0, 0, 0};
        if (compression == 3 || compression == 6)
        {
            // After a 40-byte header, or inside a V2-V5 one: the same offset.
            const size_t at = 14 + 40;
            const int count = headerSize >= 56 || compression == 6 ? 4 : 3;
            if (at + static_cast<size_t>(count) * 4 > bytes.size()) throw std::runtime_error("The BMP's channel masks are missing.");
            for (int i = 0; i < count; ++i) masks[i] = Le32(p + at + static_cast<size_t>(i) * 4);
        }
        else if (bits == 16) { masks[0] = 0x7c00; masks[1] = 0x03e0; masks[2] = 0x001f; }
        else if (bits == 32) { masks[0] = 0x00ff0000; masks[1] = 0x0000ff00; masks[2] = 0x000000ff; masks[3] = 0xff000000; }
        std::vector<std::array<uint8_t, 4>> palette;
        if (bits <= 8)
        {
            if (bits != 1 && bits != 4 && bits != 8) throw std::runtime_error("The BMP's bit depth " + std::to_string(bits) + " is not supported.");
            const size_t entry = headerSize == 12 ? 3 : 4;
            const size_t count = colours ? colours : (size_t{1} << bits);
            const size_t at = 14 + static_cast<size_t>(headerSize);
            if (count > 256 || at + count * entry > bytes.size()) throw std::runtime_error("The BMP's palette is truncated.");
            for (size_t i = 0; i < count; ++i) palette.push_back({p[at + i * entry + 2], p[at + i * entry + 1], p[at + i * entry], 255});
        }
        else if (bits != 16 && bits != 24 && bits != 32) throw std::runtime_error("The BMP's bit depth " + std::to_string(bits) + " is not supported.");
        const size_t stride = ((static_cast<size_t>(width) * static_cast<size_t>(bits) + 31) / 32) * 4;
        if (static_cast<size_t>(offset) + stride * static_cast<size_t>(height) > bytes.size()) throw std::runtime_error("The BMP's pixels are truncated.");
        auto channel = [](uint32_t value, uint32_t mask) -> uint8_t
        {
            if (!mask) return 0;
            int shift = 0;
            while (!((mask >> shift) & 1)) ++shift;
            const uint32_t max = mask >> shift;
            return static_cast<uint8_t>(((value & mask) >> shift) * 255 / max);
        };
        Rgba image(width, height);
        bool anyAlpha = false;
        for (int y = 0; y < height; ++y)
        {
            const uint8_t* row = p + offset + stride * static_cast<size_t>(topDown ? y : height - 1 - y);
            for (int x = 0; x < width; ++x)
            {
                uint8_t* out = image.At(x, y);
                if (bits <= 8)
                {
                    const size_t bit = static_cast<size_t>(x) * static_cast<size_t>(bits);
                    const int index = (row[bit / 8] >> (8 - bits - static_cast<int>(bit % 8))) & ((1 << bits) - 1);
                    if (static_cast<size_t>(index) >= palette.size()) throw std::runtime_error("The BMP uses a colour its palette does not have.");
                    std::memcpy(out, palette[static_cast<size_t>(index)].data(), 4);
                }
                else if (bits == 24) { const uint8_t* px = row + static_cast<size_t>(x) * 3; out[0] = px[2]; out[1] = px[1]; out[2] = px[0]; out[3] = 255; }
                else
                {
                    const uint32_t v = bits == 16 ? Le16(row + static_cast<size_t>(x) * 2) : Le32(row + static_cast<size_t>(x) * 4);
                    out[0] = channel(v, masks[0]); out[1] = channel(v, masks[1]); out[2] = channel(v, masks[2]);
                    out[3] = masks[3] ? channel(v, masks[3]) : 255;
                    anyAlpha = anyAlpha || (masks[3] && out[3] != 0);
                }
            }
        }
        // Many 32-bit BMPs leave the alpha byte zero: they mean opaque.
        if (bits == 32 && masks[3] && !anyAlpha)
            for (size_t i = 3; i < image.pixels.size(); i += 4) image.pixels[i] = 255;
        return image;
    }
    // 24-bit, or 32-bit (BI_BITFIELDS with an alpha mask) when the image has alpha.
    inline std::string EncodeBmp(const Rgba& image)
    {
        using namespace Detail;
        if (image.Empty()) throw std::runtime_error("An empty image cannot be written as a BMP.");
        const bool alpha = !image.Opaque();
        const int bits = alpha ? 32 : 24;
        const size_t stride = ((static_cast<size_t>(image.width) * static_cast<size_t>(bits) + 31) / 32) * 4;
        const uint32_t headerSize = alpha ? 108 : 40, offset = 14 + headerSize;
        std::string out = "BM";
        PutLe32(out, static_cast<uint32_t>(offset + stride * static_cast<size_t>(image.height)));
        PutLe32(out, 0);
        PutLe32(out, offset);
        PutLe32(out, headerSize);
        PutLe32(out, static_cast<uint32_t>(image.width));
        PutLe32(out, static_cast<uint32_t>(image.height));
        PutLe16(out, 1);
        PutLe16(out, static_cast<uint32_t>(bits));
        PutLe32(out, alpha ? 3 : 0);
        PutLe32(out, static_cast<uint32_t>(stride * static_cast<size_t>(image.height)));
        PutLe32(out, 2835);
        PutLe32(out, 2835);
        PutLe32(out, 0);
        PutLe32(out, 0);
        if (alpha)
        {
            PutLe32(out, 0x00ff0000); PutLe32(out, 0x0000ff00); PutLe32(out, 0x000000ff); PutLe32(out, 0xff000000);
            PutLe32(out, 0x73524742); // 'sRGB'
            out.append(48, '\0');     // endpoints and gamma, unused for sRGB
        }
        std::string row(stride, '\0');
        for (int y = image.height - 1; y >= 0; --y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const uint8_t* px = image.At(x, y);
                const size_t at = static_cast<size_t>(x) * static_cast<size_t>(bits / 8);
                row[at] = static_cast<char>(px[2]); row[at + 1] = static_cast<char>(px[1]); row[at + 2] = static_cast<char>(px[0]);
                if (alpha) row[at + 3] = static_cast<char>(px[3]);
            }
            out += row;
        }
        return out;
    }

    // --- TGA -----------------------------------------------------------------
    inline Rgba DecodeTga(const std::string& bytes)
    {
        using namespace Detail;
        const auto* p = Bytes(bytes);
        if (bytes.size() < 18) throw std::runtime_error("Not a TGA file (too short).");
        const int idLength = p[0], mapType = p[1], type = p[2];
        const int mapFirst = Le16(p + 3), mapLength = Le16(p + 5), mapBits = p[7];
        const int width = Le16(p + 12), height = Le16(p + 14), bits = p[16], descriptor = p[17];
        const bool rle = type >= 9;
        const int base = rle ? type - 8 : type;
        if (base != 1 && base != 2 && base != 3) throw std::runtime_error("This TGA type (" + std::to_string(type) + ") is not supported.");
        if (width <= 0 || height <= 0) throw std::runtime_error("The TGA has no pixels.");
        if (base == 2 && bits != 15 && bits != 16 && bits != 24 && bits != 32) throw std::runtime_error("The TGA's bit depth " + std::to_string(bits) + " is not supported.");
        if ((base == 1 || base == 3) && bits != 8) throw std::runtime_error("Only 8-bit grey and colour-mapped TGAs are supported.");
        size_t at = 18 + static_cast<size_t>(idLength);
        std::vector<std::array<uint8_t, 4>> palette;
        if (mapType == 1)
        {
            const size_t entry = static_cast<size_t>((mapBits + 7) / 8);
            if (at + entry * static_cast<size_t>(mapLength) > bytes.size()) throw std::runtime_error("The TGA's colour map is truncated.");
            for (int i = 0; i < mapLength; ++i)
            {
                const uint8_t* e = p + at + entry * static_cast<size_t>(i);
                if (entry >= 3) palette.push_back({e[2], e[1], e[0], entry == 4 ? e[3] : uint8_t{255}});
                else { const int v = e[0] | (e[1] << 8); palette.push_back({static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31), static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31), static_cast<uint8_t>((v & 31) * 255 / 31), 255}); }
            }
            at += entry * static_cast<size_t>(mapLength);
        }
        if (base == 1 && palette.empty()) throw std::runtime_error("The colour-mapped TGA has no colour map.");
        const size_t bpp = static_cast<size_t>((bits + 7) / 8);
        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        Rgba image(width, height);
        const bool alphaBits = (descriptor & 15) != 0;
        auto pixel = [&](const uint8_t* src, uint8_t* out)
        {
            if (base == 3) { out[0] = out[1] = out[2] = src[0]; out[3] = 255; return; }
            if (base == 1)
            {
                const int index = src[0] - mapFirst;
                if (index < 0 || static_cast<size_t>(index) >= palette.size()) throw std::runtime_error("The TGA uses a colour its colour map does not have.");
                std::memcpy(out, palette[static_cast<size_t>(index)].data(), 4);
                return;
            }
            if (bpp == 2)
            {
                const int v = src[0] | (src[1] << 8);
                out[0] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31); out[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31); out[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
                out[3] = (bits == 16 && alphaBits && !(v & 0x8000)) ? 0 : 255;
                return;
            }
            out[0] = src[2]; out[1] = src[1]; out[2] = src[0];
            out[3] = bpp == 4 ? src[3] : 255;
        };
        std::vector<uint8_t> decoded(count * 4);
        size_t written = 0;
        while (written < count)
        {
            if (!rle)
            {
                if (at + bpp > bytes.size()) throw std::runtime_error("The TGA's pixels are truncated.");
                pixel(p + at, &decoded[written * 4]);
                at += bpp;
                ++written;
                continue;
            }
            if (at >= bytes.size()) throw std::runtime_error("The TGA's pixels are truncated.");
            const int packet = p[at++];
            const size_t run = static_cast<size_t>((packet & 0x7f) + 1);
            if (written + run > count) throw std::runtime_error("The TGA's run-length data is corrupt.");
            if (packet & 0x80)
            {
                if (at + bpp > bytes.size()) throw std::runtime_error("The TGA's pixels are truncated.");
                std::array<uint8_t, 4> one{};
                pixel(p + at, one.data());
                at += bpp;
                for (size_t i = 0; i < run; ++i) std::memcpy(&decoded[(written + i) * 4], one.data(), 4);
            }
            else
            {
                if (at + bpp * run > bytes.size()) throw std::runtime_error("The TGA's pixels are truncated.");
                for (size_t i = 0; i < run; ++i, at += bpp) pixel(p + at, &decoded[(written + i) * 4]);
            }
            written += run;
        }
        // An alpha channel that is zero everywhere was never authored.
        if (bpp == 4 && !alphaBits)
        {
            bool any = false;
            for (size_t i = 3; i < decoded.size() && !any; i += 4) any = decoded[i] != 0;
            if (!any) for (size_t i = 3; i < decoded.size(); i += 4) decoded[i] = 255;
        }
        const bool topDown = (descriptor & 0x20) != 0, rightToLeft = (descriptor & 0x10) != 0;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const int sx = rightToLeft ? width - 1 - x : x, sy = topDown ? y : height - 1 - y;
                std::memcpy(image.At(x, y), &decoded[(static_cast<size_t>(sy) * static_cast<size_t>(width) + static_cast<size_t>(sx)) * 4], 4);
            }
        return image;
    }
    // 24-bit, or 32-bit with 8 alpha bits when the image has alpha; bottom-up
    // rows as most tools (and the engine's importer) expect. rle compresses.
    inline std::string EncodeTga(const Rgba& image, bool rle = false)
    {
        using namespace Detail;
        if (image.Empty() || image.width > 65535 || image.height > 65535) throw std::runtime_error("The image cannot be written as a TGA.");
        const bool alpha = !image.Opaque();
        const size_t bpp = alpha ? 4 : 3;
        std::string out(18, '\0');
        out[2] = static_cast<char>(rle ? 10 : 2);
        out[12] = static_cast<char>(image.width & 0xff); out[13] = static_cast<char>(image.width >> 8);
        out[14] = static_cast<char>(image.height & 0xff); out[15] = static_cast<char>(image.height >> 8);
        out[16] = static_cast<char>(bpp * 8);
        out[17] = static_cast<char>(alpha ? 8 : 0);
        auto put = [&](const uint8_t* px) { out += static_cast<char>(px[2]); out += static_cast<char>(px[1]); out += static_cast<char>(px[0]); if (alpha) out += static_cast<char>(px[3]); };
        auto same = [&](const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, bpp) == 0; };
        for (int y = image.height - 1; y >= 0; --y)
        {
            if (!rle) { for (int x = 0; x < image.width; ++x) put(image.At(x, y)); continue; }
            for (int x = 0; x < image.width;)
            {
                int run = 1;
                while (x + run < image.width && run < 128 && same(image.At(x, y), image.At(x + run, y))) ++run;
                if (run > 1) { out += static_cast<char>(0x80 | (run - 1)); put(image.At(x, y)); x += run; continue; }
                int raw = 1;
                while (x + raw < image.width && raw < 128 && !(x + raw + 1 < image.width && same(image.At(x + raw, y), image.At(x + raw + 1, y)))) ++raw;
                out += static_cast<char>(raw - 1);
                for (int i = 0; i < raw; ++i) put(image.At(x + i, y));
                x += raw;
            }
        }
        return out;
    }

    // --- Any of them ---------------------------------------------------------
    // "png", "bmp", "tga", or the name of a format this reader refuses
    // ("jpeg", "gif", "webp", "dds"), or "" when the bytes say nothing.
    inline std::string Format(const std::string& bytes)
    {
        if (IsPng(bytes)) return "png";
        if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return "bmp";
        if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xFF && static_cast<uint8_t>(bytes[1]) == 0xD8 && static_cast<uint8_t>(bytes[2]) == 0xFF) return "jpeg";
        if (bytes.compare(0, 4, "GIF8") == 0) return "gif";
        if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) return "webp";
        if (bytes.compare(0, 4, "DDS ") == 0) return "dds";
        // TGA has no signature: check the header is plausible.
        if (bytes.size() >= 18)
        {
            const auto* p = Detail::Bytes(bytes);
            const int type = p[2], bits = p[16];
            const bool typeOk = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
            const bool bitsOk = bits == 8 || bits == 15 || bits == 16 || bits == 24 || bits == 32;
            if (typeOk && bitsOk && p[1] <= 1 && Detail::Le16(p + 12) > 0 && Detail::Le16(p + 14) > 0) return "tga";
        }
        return {};
    }
    inline Rgba Decode(const std::string& bytes)
    {
        const auto format = Format(bytes);
        if (format == "png") return DecodePng(bytes);
        if (format == "bmp") return DecodeBmp(bytes);
        if (format == "tga") return DecodeTga(bytes);
        if (format.empty()) throw std::runtime_error("The file is not a PNG, BMP or TGA image.");
        throw std::runtime_error("The image is " + format + ", which this reader does not decode; convert it to PNG first (any image editor).");
    }

    // --- Resizing ------------------------------------------------------------
    enum class Filter { Box, Bilinear, Nearest };
    inline Filter FilterNamed(const std::string& name)
    {
        if (name == "box" || name == "area" || name.empty()) return Filter::Box;
        if (name == "bilinear" || name == "linear") return Filter::Bilinear;
        if (name == "nearest" || name == "point") return Filter::Nearest;
        throw std::runtime_error("Unknown resize filter \"" + name + "\"; use box, bilinear or nearest.");
    }
    // Colours are averaged weighted by alpha, so transparent pixels do not
    // darken the edges of what is left.
    inline Rgba Resize(const Rgba& source, int width, int height, Filter filter = Filter::Box)
    {
        if (source.Empty()) throw std::runtime_error("Cannot resize an empty image.");
        if (width <= 0 || height <= 0) throw std::runtime_error("Resize needs a positive width and height.");
        Rgba out(width, height);
        if (width == source.width && height == source.height) { out.pixels = source.pixels; return out; }
        const double sx = static_cast<double>(source.width) / width, sy = static_cast<double>(source.height) / height;
        auto store = [](uint8_t* px, double r, double g, double b, double a, double weight)
        {
            if (weight <= 0) { px[0] = px[1] = px[2] = px[3] = 0; return; }
            const double alpha = a / weight;
            const double scale = a > 1e-9 ? 1.0 / a : 0.0;
            px[0] = static_cast<uint8_t>(std::clamp(std::lround(r * scale), 0L, 255L));
            px[1] = static_cast<uint8_t>(std::clamp(std::lround(g * scale), 0L, 255L));
            px[2] = static_cast<uint8_t>(std::clamp(std::lround(b * scale), 0L, 255L));
            px[3] = static_cast<uint8_t>(std::clamp(std::lround(alpha), 0L, 255L));
        };
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                uint8_t* px = out.At(x, y);
                if (filter == Filter::Nearest || (filter == Filter::Box && sx <= 1 && sy <= 1))
                {
                    const int ix = std::min(source.width - 1, static_cast<int>((x + 0.5) * sx)), iy = std::min(source.height - 1, static_cast<int>((y + 0.5) * sy));
                    std::memcpy(px, source.At(ix, iy), 4);
                    continue;
                }
                double r = 0, g = 0, b = 0, a = 0, weight = 0;
                if (filter == Filter::Box)
                {
                    // Area average over the source rectangle this pixel covers.
                    const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
                    for (int iy = static_cast<int>(y0); iy < std::min(source.height, static_cast<int>(std::ceil(y1))); ++iy)
                    {
                        const double wy = std::min<double>(iy + 1, y1) - std::max<double>(iy, y0);
                        if (wy <= 0) continue;
                        for (int ix = static_cast<int>(x0); ix < std::min(source.width, static_cast<int>(std::ceil(x1))); ++ix)
                        {
                            const double wx = std::min<double>(ix + 1, x1) - std::max<double>(ix, x0);
                            if (wx <= 0) continue;
                            const uint8_t* s = source.At(ix, iy);
                            const double w = wx * wy, wa = w * s[3];
                            r += s[0] * wa; g += s[1] * wa; b += s[2] * wa; a += wa; weight += w;
                        }
                    }
                }
                else
                {
                    const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, source.width - 1.0), fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, source.height - 1.0);
                    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
                    const int jx = std::min(ix + 1, source.width - 1), jy = std::min(iy + 1, source.height - 1);
                    const double tx = fx - ix, ty = fy - iy;
                    const std::pair<const uint8_t*, double> taps[4] = {{source.At(ix, iy), (1 - tx) * (1 - ty)}, {source.At(jx, iy), tx * (1 - ty)},
                                                                      {source.At(ix, jy), (1 - tx) * ty}, {source.At(jx, jy), tx * ty}};
                    for (const auto& [s, w] : taps)
                    {
                        const double wa = w * s[3];
                        r += s[0] * wa; g += s[1] * wa; b += s[2] * wa; a += wa; weight += w;
                    }
                }
                store(px, r, g, b, a, weight);
            }
        return out;
    }
    // The image scaled down (never up) so its longer side is at most maxEdge.
    inline Rgba FitWithin(const Rgba& source, int maxEdge, Filter filter = Filter::Box)
    {
        if (maxEdge <= 0 || (source.width <= maxEdge && source.height <= maxEdge)) return source;
        const double scale = static_cast<double>(maxEdge) / std::max(source.width, source.height);
        return Resize(source, std::max(1, static_cast<int>(std::lround(source.width * scale))), std::max(1, static_cast<int>(std::lround(source.height * scale))), filter);
    }
    inline Rgba Crop(const Rgba& source, int x, int y, int width, int height)
    {
        if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > source.width || y + height > source.height)
            throw std::runtime_error("The crop rectangle lies outside the " + std::to_string(source.width) + "x" + std::to_string(source.height) + " image.");
        Rgba out(width, height);
        for (int row = 0; row < height; ++row) std::memcpy(out.At(0, row), source.At(x, y + row), static_cast<size_t>(width) * 4);
        return out;
    }
    // The power of two nearest to n (ties go up), for texture sizes.
    inline int NearestPowerOfTwo(int n)
    {
        if (n <= 1) return 1;
        int p = 1;
        while (p < n && p < (1 << 14)) p <<= 1;
        return (p - n) <= (n - p / 2) ? p : p / 2;
    }
}
