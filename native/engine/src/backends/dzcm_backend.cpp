// DZCM - DevZIP Context-Mixing backend.
//
// A from-scratch binary context-mixing compressor in the PAQ/lpaq family,
// written for DevZIP. It predicts the input one bit at a time by mixing several
// finite-context models, a match model, and a word model through a logistic
// (online gradient) mixer, refines the result with an SSE/APM stage, and codes
// each bit with a 32-bit binary arithmetic coder.
//
// The techniques (logistic mixing, secondary symbol estimation, binary
// arithmetic coding) are long-published and unpatented; this is an independent
// implementation so DevZIP can own a permissively licensed strong coder and,
// uniquely, feed it structural hints from the surrounding transform pipeline.
//
// Determinism: every prediction is pure integer math and the decoder replays
// the exact same model updates as the encoder, so archives are portable and
// round-trip byte-exact.

#include "devzip/archive_format.h"
#include "devzip/backend.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace devzip {
namespace {

using U8 = std::uint8_t;
using U32 = std::uint32_t;
using U64 = std::uint64_t;

// ---------------------------------------------------------------------------
// stretch / squash lookup tables.
//   squash(d) = 4096 / (1 + e^(-d/256)),  d in [-2047, 2047] -> p in [1, 4095]
//   stretch(p) = squash^-1(p)
// ---------------------------------------------------------------------------
struct Tables {
  short squash_t[4096];   // index d + 2048
  short stretch_t[4096];  // index p (0..4095)
  Tables() {
    for (int d = -2047; d <= 2047; ++d) {
      double v = 4096.0 / (1.0 + std::exp(-d / 256.0));
      int p = static_cast<int>(v + 0.5);
      if (p < 1) p = 1;
      if (p > 4095) p = 4095;
      squash_t[d + 2048] = static_cast<short>(p);
    }
    int pi = 0;
    for (int d = -2047; d <= 2047; ++d) {
      int p = squash_t[d + 2048];
      for (; pi <= p && pi < 4096; ++pi) stretch_t[pi] = static_cast<short>(d);
    }
    for (; pi < 4096; ++pi) stretch_t[pi] = 2047;
  }
};
const Tables g_tables;

inline int squash(int d) {
  if (d < -2047) d = -2047;
  if (d > 2047) d = 2047;
  return g_tables.squash_t[d + 2048];
}
inline int stretch(int p) {
  if (p < 0) p = 0;
  if (p > 4095) p = 4095;
  return g_tables.stretch_t[p];
}

inline U32 hash_ctx(U64 v, U32 salt) {
  v += salt * 0x100000001B3ULL;
  v *= 0x9E3779B97F4A7C15ULL;
  v ^= v >> 29;
  v *= 0xBF58476D1CE4E5B9ULL;
  v ^= v >> 32;
  return static_cast<U32>(v);
}

// ---------------------------------------------------------------------------
// Logistic mixer: pr = squash( sum_i w_i * stretch(p_i) ).  Weights are trained
// online by the prediction error and selected by a small context.
// ---------------------------------------------------------------------------
constexpr int kMaxInputs = 16;

class Mixer {
 public:
  Mixer(int inputs, int contexts)
      : inputs_(inputs), weights_(static_cast<std::size_t>(inputs) * contexts, 0) {}

  void add(int stretched) { tx_[count_++] = stretched; }
  void set_context(int ctx) { base_ = ctx * inputs_; }

  int mix() {
    long long dot = 0;
    for (int i = 0; i < count_; ++i) dot += static_cast<long long>(weights_[base_ + i]) * tx_[i];
    pr_ = squash(static_cast<int>(dot >> 16));
    return pr_;
  }

  void update(int y) {
    const int err = ((y << 12) - pr_) * 7;
    for (int i = 0; i < count_; ++i) {
      weights_[base_ + i] += (tx_[i] * err + 0x8000) >> 16;
    }
    count_ = 0;
  }

 private:
  int inputs_;
  int count_ = 0;
  int base_ = 0;
  int pr_ = 2048;
  int tx_[kMaxInputs] = {0};
  std::vector<int> weights_;
};

// ---------------------------------------------------------------------------
// Adaptive probability map (SSE): refines a probability through an interpolated
// table selected by a context.
// ---------------------------------------------------------------------------
class Apm {
 public:
  explicit Apm(int contexts) : t_(static_cast<std::size_t>(contexts) * 33) {
    for (int i = 0; i < contexts; ++i)
      for (int j = 0; j < 33; ++j)
        t_[i * 33 + j] = static_cast<std::uint16_t>(squash((j - 16) * 128) * 16);
  }

  int refine(int pr, int ctx) {
    const int s = stretch(pr) + 2048;          // 0..4095
    const int w = s & 127;
    idx_ = ctx * 33 + (s >> 7);
    return (t_[idx_] * (128 - w) + t_[idx_ + 1] * w) >> 11;
  }

  void update(int y) {
    const int g = (y << 16) + (y << 7) - y - y;  // target in 16-bit domain
    t_[idx_] += (g - t_[idx_]) >> 7;
    t_[idx_ + 1] += (g - t_[idx_ + 1]) >> 7;
  }

 private:
  int idx_ = 0;
  std::vector<std::uint16_t> t_;
};

// ---------------------------------------------------------------------------
// Match model: predicts the byte that followed the most recent occurrence of
// the current high-order context, weighted by how long the match has held.
// ---------------------------------------------------------------------------
class MatchModel {
 public:
  explicit MatchModel(int bits) : mask_((1u << bits) - 1), table_(static_cast<std::size_t>(mask_) + 1, 0) {}

  // Stretched contribution for the current bit (0 if no active prediction).
  int predict(int c0, int bpos, const std::vector<U8>& hist) {
    if (len_ == 0 || ptr_ >= hist.size()) return 0;
    pred_byte_ = hist[ptr_];
    const int pbit = (pred_byte_ >> (7 - bpos)) & 1;
    int strength = (len_ > 28 ? 28 : len_) * 64 + 64;
    if (strength > 2047) strength = 2047;
    return pbit ? strength : -strength;
  }

  // Called once per finished byte.
  void update_byte(U8 byte, const std::vector<U8>& hist) {
    const std::size_t len = hist.size();  // history already includes `byte`
    if (len_ > 0 && ptr_ < len && hist[ptr_] == byte) {
      ++ptr_;
      if (len_ < 65535) ++len_;
    } else {
      len_ = 0;
    }
    if (len >= kMinLen) {
      hash_ = 0;
      for (std::size_t i = len - kMinLen; i < len; ++i) hash_ = hash_ * 0x100000001B3ULL + hist[i] + 1;
      const U32 slot = static_cast<U32>(hash_) & mask_;
      if (len_ == 0) {
        const U32 cand = table_[slot];
        if (cand != 0 && cand < len) {
          ptr_ = cand;
          len_ = 1;
        }
      }
      table_[slot] = static_cast<U32>(len);
    }
  }

 private:
  static constexpr std::size_t kMinLen = 4;
  U32 mask_;
  std::vector<U32> table_;
  U64 hash_ = 0;
  std::size_t ptr_ = 0;
  int len_ = 0;
  int pred_byte_ = 0;
};

// ---------------------------------------------------------------------------
// Predictor.
//
// Architecture (PAQ/lpaq family): a bank of finite-context models (orders
// 0,1,2,3,4,5,6,8), a word model for text, two sparse models for structured
// data, and a match model each emit a probability; a context-selected logistic
// mixer combines their stretched predictions and two SSE/APM stages refine the
// result.  Produces P(next bit == 1), 12-bit.  All integer, fully deterministic.
// ---------------------------------------------------------------------------
class Predictor {
 public:
  Predictor()
      : mixer_(kInputs, 512),
        apm1_(256),
        apm2_(0x10000),
        match_(22) {
    ctx_[0].assign(512, 0x80000000u);  // order 0: keyed by c0 only
    for (int m = 1; m < kCounters; ++m) ctx_[m].assign(kTableSize, 0x80000000u);
    refresh_hashes();
  }

  int predict() {
    const U32 cmix = static_cast<U32>(c0_) * 0x9E3779B1u;
    cell_[0] = &ctx_[0][static_cast<U32>(c0_) & 511];
    for (int m = 1; m < kCounters; ++m) {
      cell_[m] = &ctx_[m][(base_hash_[m] ^ cmix) & kTableMask];
    }

    mixer_.set_context(((c4_ & 1) << 8) | (c0_ & 255));
    for (int m = 0; m < kCounters; ++m) mixer_.add(stretch(static_cast<int>(*cell_[m] >> 20)));
    mixer_.add(match_.predict(c0_, bpos_, hist_));

    int p = mixer_.mix();
    p = (p + 3 * apm1_.refine(p, c0_ & 255)) >> 2;
    p = (p + 3 * apm2_.refine(p, static_cast<int>(((c4_ & 0xff) << 8) | c0_) & 0xffff)) >> 2;
    if (p < 1) p = 1;
    if (p > 4095) p = 4095;
    return p;
  }

  void update(int y) {
    for (int m = 0; m < kCounters; ++m) update_counter(cell_[m], y);
    mixer_.update(y);
    apm1_.update(y);
    apm2_.update(y);

    c0_ = (c0_ << 1) | y;
    ++bpos_;
    if (c0_ >= 256) {
      const U8 byte = static_cast<U8>(c0_ & 0xff);
      c0_ = 1;
      bpos_ = 0;
      hist_.push_back(byte);
      c4_ = (c4_ << 8) | byte;
      cx_ = (cx_ << 8) | byte;
      if (std::isalnum(byte)) {
        word_hash_ = word_hash_ * 0x100000001B3ULL + byte + 1;
      } else {
        word_hash_ = 0;
      }
      match_.update_byte(byte, hist_);
      refresh_hashes();
    }
  }

 private:
  static void update_counter(U32* cell, int y) {
    U32 c = *cell;
    int n = c & 1023;
    int p = c >> 10;  // 22-bit probability of 1
    const int target = y ? ((1 << 22) - 1) : 0;
    p += (target - p) / (n + 2);
    if (n < 1023) ++n;
    *cell = (static_cast<U32>(p) << 10) | n;
  }

  void refresh_hashes() {
    // base_hash_[0] (order 0) is unused; cell 0 is keyed by c0 directly.
    base_hash_[1] = hash_ctx(cx_ & 0xffULL, 1);                  // order 1
    base_hash_[2] = hash_ctx(cx_ & 0xffffULL, 2);                // order 2
    base_hash_[3] = hash_ctx(cx_ & 0xffffffULL, 3);              // order 3
    base_hash_[4] = hash_ctx(cx_ & 0xffffffffULL, 4);            // order 4
    base_hash_[5] = hash_ctx(cx_ & 0xffffffffffULL, 5);          // order 5
    base_hash_[6] = hash_ctx(cx_ & 0xffffffffffffULL, 6);        // order 6
    base_hash_[7] = hash_ctx(cx_, 8);                            // order 8
    base_hash_[8] = hash_ctx(word_hash_, 101);                   // word
    base_hash_[9] = hash_ctx((cx_ >> 8) & 0xffffULL, 202);       // sparse: skip last byte
    base_hash_[10] = hash_ctx((cx_ & 0xffULL) | ((cx_ >> 8) & 0xff00ULL), 303);  // sparse 1,3
  }

  static constexpr int kCounters = 11;            // context-counter models
  static constexpr int kInputs = kCounters + 1;   // + match model
  static constexpr U32 kTableBits = 22;
  static constexpr U32 kTableSize = 1u << kTableBits;
  static constexpr U32 kTableMask = kTableSize - 1;

  std::vector<U32> ctx_[kCounters];
  U32* cell_[kCounters] = {nullptr};
  U32 base_hash_[kCounters] = {0};

  Mixer mixer_;
  Apm apm1_;
  Apm apm2_;
  MatchModel match_;

  std::vector<U8> hist_;
  U64 cx_ = 0;          // last 8 bytes
  U32 c4_ = 0;          // last 4 bytes
  U64 word_hash_ = 0;
  int c0_ = 1;          // partial current byte (leading-1 sentinel)
  int bpos_ = 0;
};

// ---------------------------------------------------------------------------
// Binary arithmetic coder (carryless 32-bit range coder).
// ---------------------------------------------------------------------------
class Encoder {
 public:
  explicit Encoder(std::vector<std::byte>& out) : out_(out) {}

  void encode(int bit, int p) {  // p = P(bit==1), 12-bit, in [1,4095]
    const U32 xmid = x1_ + ((x2_ - x1_) >> 12) * static_cast<U32>(p);
    if (bit) x2_ = xmid; else x1_ = xmid + 1;
    while (((x1_ ^ x2_) & 0xff000000u) == 0) {
      out_.push_back(static_cast<std::byte>(x2_ >> 24));
      x1_ <<= 8;
      x2_ = (x2_ << 8) | 255;
    }
  }

  void flush() {
    // Emit the four bytes of x1 so the decoder can prime its window.
    for (int i = 0; i < 4; ++i) {
      out_.push_back(static_cast<std::byte>(x1_ >> 24));
      x1_ <<= 8;
    }
  }

 private:
  std::vector<std::byte>& out_;
  U32 x1_ = 0;
  U32 x2_ = 0xffffffffu;
};

class Decoder {
 public:
  explicit Decoder(std::span<const std::byte> in) : in_(in) {
    for (int i = 0; i < 4; ++i) x_ = (x_ << 8) | next();
  }

  int decode(int p) {
    const U32 xmid = x1_ + ((x2_ - x1_) >> 12) * static_cast<U32>(p);
    int bit;
    if (x_ <= xmid) { bit = 1; x2_ = xmid; } else { bit = 0; x1_ = xmid + 1; }
    while (((x1_ ^ x2_) & 0xff000000u) == 0) {
      x1_ <<= 8;
      x2_ = (x2_ << 8) | 255;
      x_ = (x_ << 8) | next();
    }
    return bit;
  }

 private:
  U32 next() { return pos_ < in_.size() ? static_cast<U32>(static_cast<U8>(in_[pos_++])) : 0; }
  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
  U32 x1_ = 0;
  U32 x2_ = 0xffffffffu;
  U32 x_ = 0;
};

// ---------------------------------------------------------------------------
// Backend wrapper.
// ---------------------------------------------------------------------------
constexpr std::byte kStored{0x00};
constexpr std::byte kCoded{0x01};

class DzcmBackend final : public CompressionBackend {
 public:
  BackendStamp stamp() const override { return {"dzcm", "1"}; }

  CompressionResponse compress(const CompressionRequest& request) override {
    CompressionResponse response;
    response.raw_size = static_cast<std::uint64_t>(request.input.size());
    response.checksum = checksum_bytes(request.input);

    if (request.input.empty()) {
      response.encoded.push_back(kStored);
      return response;
    }

    std::vector<std::byte> coded;
    coded.reserve(request.input.size() / 2 + 16);
    coded.push_back(kCoded);
    {
      Encoder enc(coded);
      Predictor pr;
      for (std::byte b : request.input) {
        const int byte = static_cast<int>(static_cast<U8>(b));
        for (int i = 7; i >= 0; --i) {
          const int bit = (byte >> i) & 1;
          enc.encode(bit, pr.predict());
          pr.update(bit);
        }
      }
      enc.flush();
    }

    // Fall back to a stored copy if the model failed to beat verbatim.
    if (coded.size() >= request.input.size() + 1) {
      response.encoded.clear();
      response.encoded.reserve(request.input.size() + 1);
      response.encoded.push_back(kStored);
      response.encoded.insert(response.encoded.end(), request.input.begin(), request.input.end());
      return response;
    }
    response.encoded = std::move(coded);
    return response;
  }

  std::vector<std::byte> decompress(std::span<const std::byte> encoded,
                                    std::uint64_t expected_raw_size) const override {
    if (encoded.empty()) throw std::runtime_error("dzcm: empty payload");
    const auto frame = encoded.front();
    const auto body = encoded.subspan(1);

    if (frame == kStored) {
      return std::vector<std::byte>(body.begin(), body.end());
    }
    if (frame != kCoded) throw std::runtime_error("dzcm: unknown frame byte");

    std::vector<std::byte> out;
    out.reserve(static_cast<std::size_t>(expected_raw_size));
    Decoder dec(body);
    Predictor pr;
    for (std::uint64_t n = 0; n < expected_raw_size; ++n) {
      int byte = 0;
      for (int i = 0; i < 8; ++i) {
        const int bit = dec.decode(pr.predict());
        pr.update(bit);
        byte = (byte << 1) | bit;
      }
      out.push_back(static_cast<std::byte>(byte));
    }
    return out;
  }
};

}  // namespace

std::unique_ptr<CompressionBackend> make_dzcm_backend() {
  return std::make_unique<DzcmBackend>();
}

}  // namespace devzip
