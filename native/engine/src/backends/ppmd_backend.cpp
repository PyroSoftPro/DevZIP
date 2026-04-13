#include "devzip/backend.h"

extern "C" {
#include "Alloc.h"
#include "Ppmd7.h"
}

#include <cstddef>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace devzip {
namespace {

constexpr unsigned kPpmdOrder = 32;
constexpr UInt32 kPpmdMemSize = 256u << 20;

std::string ppmd_error(std::string_view context) {
  return std::string(context) + ": PPMd encode/decode failed";
}

void write_u32le(std::vector<std::byte>& output, UInt32 value) {
  output.push_back(static_cast<std::byte>(value & 0xFF));
  output.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
  output.push_back(static_cast<std::byte>((value >> 16) & 0xFF));
  output.push_back(static_cast<std::byte>((value >> 24) & 0xFF));
}

UInt32 read_u32le(std::span<const std::byte> input) {
  return static_cast<UInt32>(std::to_integer<unsigned char>(input[0])) |
         (static_cast<UInt32>(std::to_integer<unsigned char>(input[1])) << 8) |
         (static_cast<UInt32>(std::to_integer<unsigned char>(input[2])) << 16) |
         (static_cast<UInt32>(std::to_integer<unsigned char>(input[3])) << 24);
}

struct ByteVectorOut {
  IByteOut vt{};
  std::vector<std::byte> data;

  ByteVectorOut() { vt.Write = &Write; }

  static void Write(IByteOutPtr stream, Byte value) {
    auto* self = const_cast<ByteVectorOut*>(reinterpret_cast<const ByteVectorOut*>(stream));
    self->data.push_back(static_cast<std::byte>(value));
  }
};

struct ByteSpanIn {
  IByteIn vt{};
  std::span<const std::byte> data;
  std::size_t position = 0;
  bool extra = false;

  explicit ByteSpanIn(std::span<const std::byte> input) : data(input) { vt.Read = &Read; }

  static Byte Read(IByteInPtr stream) {
    auto* self = const_cast<ByteSpanIn*>(reinterpret_cast<const ByteSpanIn*>(stream));
    if (self->position >= self->data.size()) {
      self->extra = true;
      return 0;
    }
    return static_cast<Byte>(std::to_integer<unsigned char>(self->data[self->position++]));
  }
};

struct PpmdState {
  CPpmd7 ppmd{};
  bool allocated = false;

  PpmdState() { Ppmd7_Construct(&ppmd); }
  ~PpmdState() {
    if (allocated) {
      Ppmd7_Free(&ppmd, &g_Alloc);
    }
  }
};

class PpmdBackend final : public CompressionBackend {
 public:
  BackendStamp stamp() const override { return {"ppmd", "o32-m256m"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    PpmdState state;
    if (!Ppmd7_Alloc(&state.ppmd, kPpmdMemSize, &g_Alloc)) {
      throw std::bad_alloc();
    }
    state.allocated = true;
    Ppmd7_Init(&state.ppmd, kPpmdOrder);

    ByteVectorOut output;
    state.ppmd.rc.enc.Stream = &output.vt;
    Ppmd7z_Init_RangeEnc(&state.ppmd);
    if (!request.input.empty()) {
      const auto* begin = reinterpret_cast<const Byte*>(request.input.data());
      Ppmd7z_EncodeSymbols(&state.ppmd, begin, begin + request.input.size());
    }
    Ppmd7z_Flush_RangeEnc(&state.ppmd);

    CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = checksum_bytes(request.input);
    response.encoded.reserve(5 + output.data.size());
    response.encoded.push_back(static_cast<std::byte>(kPpmdOrder));
    write_u32le(response.encoded, kPpmdMemSize);
    response.encoded.insert(response.encoded.end(), output.data.begin(), output.data.end());
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.size() < 5) {
      throw std::runtime_error("PPMd payload is missing properties");
    }

    const unsigned order = std::to_integer<unsigned char>(encoded[0]);
    const UInt32 mem_size = read_u32le(encoded.subspan(1, 4));
    if (order < PPMD7_MIN_ORDER || order > PPMD7_MAX_ORDER ||
        mem_size < PPMD7_MIN_MEM_SIZE || mem_size > PPMD7_MAX_MEM_SIZE) {
      throw std::runtime_error("PPMd payload properties are invalid");
    }

    PpmdState state;
    if (!Ppmd7_Alloc(&state.ppmd, mem_size, &g_Alloc)) {
      throw std::bad_alloc();
    }
    state.allocated = true;
    Ppmd7_Init(&state.ppmd, order);

    const auto payload = encoded.subspan(5);
    ByteSpanIn input(payload);
    state.ppmd.rc.dec.Stream = &input.vt;
    if (!Ppmd7z_RangeDec_Init(&state.ppmd.rc.dec) || input.extra) {
      throw std::runtime_error(ppmd_error("PPMd decoder init failed"));
    }

    std::vector<std::byte> decoded(static_cast<std::size_t>(expected_raw_size));
    for (std::size_t index = 0; index < decoded.size(); ++index) {
      const int sym = Ppmd7z_DecodeSymbol(&state.ppmd);
      if (input.extra || sym < 0) {
        throw std::runtime_error(ppmd_error("PPMd decompression failed"));
      }
      decoded[index] = static_cast<std::byte>(static_cast<unsigned char>(sym));
    }

    if (input.extra ||
        !Ppmd7z_RangeDec_IsFinishedOK(&state.ppmd.rc.dec) ||
        input.position != payload.size()) {
      throw std::runtime_error(ppmd_error("PPMd stream did not finish cleanly"));
    }

    return decoded;
  }
};

}  // namespace

std::unique_ptr<CompressionBackend> make_ppmd_backend() {
  return std::make_unique<PpmdBackend>();
}

}  // namespace devzip
