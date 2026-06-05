#include "devzip/backend.h"

extern "C" {
#include "Alloc.h"
#include "Lzma2Dec.h"
#include "Lzma2Enc.h"
}

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace devzip {
namespace {

UInt32 choose_dict_size(std::size_t input_size) {
  constexpr UInt32 kMinDictSize = 1u << 20;
  constexpr UInt32 kMaxDictSize = sizeof(void*) >= 8 ? (1u << 29) : (1u << 27);

  if (input_size == 0) {
    return kMinDictSize;
  }

  UInt64 target = static_cast<UInt64>(input_size);
  if (target < kMinDictSize) {
    target = kMinDictSize;
  }
  if (target > kMaxDictSize) {
    target = kMaxDictSize;
  }

  UInt32 dict_size = kMinDictSize;
  while (static_cast<UInt64>(dict_size) < target && dict_size < kMaxDictSize) {
    dict_size <<= 1;
  }

  return dict_size;
}

// LZMA's position-bits (pb) and literal-context (lc) parameters trade off
// between text and binary content.  Text streams favour pb=0 (no positional
// alignment) while machine code / structured binaries favour pb=2 (model the
// 4-byte instruction/word alignment).  LZMA2 records lc/lp/pb inside the stream
// itself, so adapting them here requires no format change and the decoder reads
// them back automatically.  We sample a prefix to classify cheaply.
struct LiteralModel {
  unsigned lc = 3;
  unsigned lp = 0;
  unsigned pb = 0;
};

LiteralModel choose_literal_model(std::span<const std::byte> input) {
  LiteralModel model;
  if (input.empty()) {
    return model;
  }

  const std::size_t sample = std::min<std::size_t>(input.size(), 256u * 1024u);
  std::size_t text_bytes = 0;
  for (std::size_t i = 0; i < sample; ++i) {
    const auto value = std::to_integer<unsigned char>(input[i]);
    const bool printable = (value >= 0x20 && value <= 0x7E) || value == 0x09 ||
                           value == 0x0A || value == 0x0D;
    if (printable) {
      ++text_bytes;
    }
  }

  const double text_ratio = static_cast<double>(text_bytes) / static_cast<double>(sample);
  if (text_ratio < 0.85) {
    // Binary / machine-code dominated: align the literal coder to 4-byte words.
    model.pb = 2;
  }
  return model;
}

std::string lzma_error_message(SRes code, std::string_view context) {
  const char* reason = "unknown error";
  switch (code) {
    case SZ_OK:
      reason = "success";
      break;
    case SZ_ERROR_MEM:
      reason = "out of memory";
      break;
    case SZ_ERROR_PARAM:
      reason = "invalid encoder parameters";
      break;
    case SZ_ERROR_OUTPUT_EOF:
      reason = "output buffer overflow";
      break;
    case SZ_ERROR_DATA:
      reason = "corrupt compressed data";
      break;
    case SZ_ERROR_INPUT_EOF:
      reason = "unexpected end of compressed data";
      break;
    case SZ_ERROR_THREAD:
      reason = "threading failure";
      break;
  }

  return std::string(context) + ": " + reason;
}

std::string zpaq_method_from_version(std::string_view version) {
  const auto marker = version.find("zpaq-m");
  if (marker == std::string_view::npos) {
    return "4";
  }
  std::string method;
  for (std::size_t index = marker + 6; index < version.size(); ++index) {
    const char ch = version[index];
    if (ch < '0' || ch > '9') {
      break;
    }
    method.push_back(ch);
  }
  return method.empty() ? "4" : method;
}

struct VectorOutStream {
  ISeqOutStream vt{};
  std::vector<std::byte> data;

  VectorOutStream() { vt.Write = &Write; }

  static size_t Write(ISeqOutStreamPtr stream, const void* buffer, size_t size) {
    auto* self = const_cast<VectorOutStream*>(reinterpret_cast<const VectorOutStream*>(stream));
    const auto* begin = static_cast<const std::byte*>(buffer);
    self->data.insert(self->data.end(), begin, begin + static_cast<std::ptrdiff_t>(size));
    return size;
  }
};

class Lzma2Backend final : public CompressionBackend {
 public:
  BackendStamp stamp() const override { return {"lzma2", "sdk-26.00"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    if (request.input.size() > static_cast<std::size_t>(std::numeric_limits<UInt64>::max())) {
      throw std::runtime_error("LZMA2 backend cannot encode payloads larger than UInt64 bytes");
    }

    CLzma2EncHandle encoder = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
    if (encoder == nullptr) {
      throw std::bad_alloc();
    }

    const auto destroy = [](CLzma2Enc* handle) {
      if (handle != nullptr) {
        Lzma2Enc_Destroy(handle);
      }
    };
    std::unique_ptr<CLzma2Enc, decltype(destroy)> guard(encoder, destroy);

    const LiteralModel literal_model = choose_literal_model(request.input);

    CLzma2EncProps props;
    Lzma2EncProps_Init(&props);
    props.lzmaProps.level = 9;
    props.lzmaProps.algo = 1;
    props.lzmaProps.dictSize = choose_dict_size(request.input.size());
    props.lzmaProps.fb = 273;
    props.lzmaProps.btMode = 1;
    props.lzmaProps.mc = 10000;
    props.lzmaProps.lc = static_cast<int>(literal_model.lc);
    props.lzmaProps.lp = static_cast<int>(literal_model.lp);
    props.lzmaProps.pb = static_cast<int>(literal_model.pb);
    props.lzmaProps.numHashBytes = 4;
    props.lzmaProps.numThreads = 1;
    props.lzmaProps.reduceSize = static_cast<UInt64>(request.input.size());
    props.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
    props.numBlockThreads_Reduced = 1;
    props.numBlockThreads_Max = 1;
    props.numTotalThreads = 1;
    props.numThreadGroups = 0;

    SRes result = Lzma2Enc_SetProps(encoder, &props);
    if (result != SZ_OK) {
      throw std::runtime_error(lzma_error_message(result, "LZMA2 encoder setup failed"));
    }

    Lzma2Enc_SetDataSize(encoder, static_cast<UInt64>(request.input.size()));
    const Byte property = Lzma2Enc_WriteProperties(encoder);

    VectorOutStream stream;
    result = Lzma2Enc_Encode2(encoder,
                              &stream.vt,
                              nullptr,
                              nullptr,
                              nullptr,
                              reinterpret_cast<const Byte*>(request.input.data()),
                              request.input.size(),
                              nullptr);
    if (result != SZ_OK) {
      throw std::runtime_error(lzma_error_message(result, "LZMA2 compression failed"));
    }

    CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = checksum_bytes(request.input);
    response.encoded.reserve(stream.data.size() + 1);
    response.encoded.push_back(static_cast<std::byte>(property));
    response.encoded.insert(response.encoded.end(), stream.data.begin(), stream.data.end());
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) {
      throw std::runtime_error("LZMA2 payload is missing stream properties");
    }
    if (expected_raw_size > static_cast<std::uint64_t>(std::numeric_limits<SizeT>::max())) {
      throw std::runtime_error("LZMA2 backend cannot decode payloads larger than SizeT bytes");
    }

    const Byte property = static_cast<Byte>(encoded.front());
    const auto compressed = encoded.subspan(1);

    std::vector<std::byte> decoded(static_cast<std::size_t>(expected_raw_size));
    Byte scratch = 0;
    Byte* output = decoded.empty() ? &scratch : reinterpret_cast<Byte*>(decoded.data());
    SizeT output_size = static_cast<SizeT>(expected_raw_size);
    SizeT source_size = static_cast<SizeT>(compressed.size());
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;

    const SRes result = Lzma2Decode(output,
                                    &output_size,
                                    reinterpret_cast<const Byte*>(compressed.data()),
                                    &source_size,
                                    property,
                                    LZMA_FINISH_END,
                                    &status,
                                    &g_Alloc);
    if (result != SZ_OK) {
      throw std::runtime_error(lzma_error_message(result, "LZMA2 decompression failed"));
    }
    if (status != LZMA_STATUS_FINISHED_WITH_MARK) {
      throw std::runtime_error("LZMA2 decompression failed: stream did not finish cleanly");
    }
    if (output_size != static_cast<SizeT>(expected_raw_size)) {
      throw std::runtime_error("LZMA2 decompression failed: decoded size mismatch");
    }
    if (source_size != static_cast<SizeT>(compressed.size())) {
      throw std::runtime_error("LZMA2 decompression failed: trailing data remained unread");
    }

    return decoded;
  }
};

}  // namespace

std::unique_ptr<CompressionBackend> make_lzma2_backend() {
  return std::make_unique<Lzma2Backend>();
}

std::unique_ptr<CompressionBackend> make_backend(const BackendStamp& stamp) {
  if (stamp.name == "libzpaq") {
    return make_libzpaq_backend();
  }
  if (stamp.name == "lzma2") {
    return make_lzma2_backend();
  }
  if (stamp.name == "ppmd") {
    return make_ppmd_backend();
  }
  if (stamp.name == "best-of-two") {
    return make_best_of_two_backend(zpaq_method_from_version(stamp.version));
  }
  if (stamp.name == "best-of-three") {
    return make_best_of_three_backend();
  }
  if (stamp.name == "best-of-n") {
    return make_best_of_n_backend(stamp.version);
  }
  if (stamp.name == "bsc") {
    return make_bsc_backend();
  }
  if (stamp.name == "selective-zpaq") {
    return make_selective_zpaq_backend();
  }

  throw std::runtime_error("Unsupported compression backend: " + stamp.name + " " + stamp.version);
}

}  // namespace devzip
