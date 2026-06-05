#include "devzip/archive_format.h"
#include "devzip/backend.h"
#include "devzip/extractor.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void print_usage() {
  std::cout
      << "DevZip CLI\n"
      << "Usage:\n"
      << "  devzip [--json] [--backend NAME] [--level L] [--verify] compress <source> [archive]\n"
      << "  devzip [--json] extract <archive> [destination]\n"
      << "  devzip [--json] verify <archive>\n"
      << "Levels (compress): fast | balanced (default) | max | insane\n"
      << "Backends:\n"
      << "  best-of-two        (default)\n"
      << "  best-of-three-ppmd\n"
      << "  selective-zpaq5\n"
      << "  lzma2\n"
      << "  ppmd\n"
      << "  libzpaq-4\n"
      << "  libzpaq-5\n";
}

std::string json_escape(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

std::filesystem::path default_extract_path(const std::filesystem::path& archive_path) {
  const auto stem = archive_path.stem();
  return archive_path.parent_path() / (stem.empty() ? std::filesystem::path("extracted") : stem);
}

std::unique_ptr<devzip::CompressionBackend> make_backend_from_name(std::string_view name) {
  if (name.empty() || name == "best-of-two") {
    return devzip::make_best_of_two_backend("4");
  }
  if (name == "best-of-three-ppmd") {
    return devzip::make_best_of_three_backend();
  }
  if (name == "selective-zpaq5") {
    return devzip::make_selective_zpaq_backend();
  }
  if (name == "lzma2") {
    return devzip::make_lzma2_backend();
  }
  if (name == "ppmd") {
    return devzip::make_ppmd_backend();
  }
  if (name == "libzpaq-4") {
    return devzip::make_libzpaq_backend("4");
  }
  if (name == "libzpaq-5") {
    return devzip::make_libzpaq_backend("5");
  }
  if (name == "bsc") {
    auto bsc = devzip::make_bsc_backend();
    if (!bsc) {
      throw std::runtime_error("libbsc backend is not available in this build");
    }
    return bsc;
  }
  if (name == "best-of-n") {
    return devzip::make_best_of_n_backend("lzma2,zpaq5,ppmd,bsc");
  }
  if (name.rfind("best-of-n:", 0) == 0) {
    return devzip::make_best_of_n_backend(std::string(name.substr(10)));
  }
  throw std::runtime_error("Unknown backend name: " + std::string(name));
}

// When the user does not pin a backend, the compression level selects how hard
// the engine works: faster single codecs at low levels, escalating best-of-N
// (adding zpaq-5 context mixing, PPMd, and libbsc BWT) at higher levels.
std::string default_backend_for_level(devzip::CompressionLevel level) {
  switch (level) {
    case devzip::CompressionLevel::Fast:
      return "lzma2";
    case devzip::CompressionLevel::Balanced:
      // lzma2 + zpaq-5 context mixing: zpaq-5 models executables and structured
      // binaries well enough to keep every category ahead of 7-Zip, while the
      // two codecs run concurrently so wall-time stays close to best-of-two.
      return "best-of-n:lzma2,zpaq5";
    case devzip::CompressionLevel::Max:
      return "best-of-n:lzma2,zpaq5,ppmd";
    case devzip::CompressionLevel::Insane:
      return devzip::bsc_backend_available() ? "best-of-n:lzma2,zpaq5,ppmd,bsc"
                                             : "best-of-n:lzma2,zpaq5,ppmd";
  }
  return "best-of-two";
}

}  // namespace

int main(int argc, char** argv) {
  bool json_output = false;
  std::string backend_name;  // empty => derive from level
  devzip::CompressionLevel level = devzip::CompressionLevel::Balanced;
  bool verify_roundtrip = false;
  if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help" ||
      std::string(argv[1]) == "-h") {
    print_usage();
    return argc < 2 ? 1 : 0;
  }

  try {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int index = 1; index < argc; ++index) {
      if (std::string(argv[index]) == "--json") {
        json_output = true;
        continue;
      }
      if (std::string(argv[index]) == "--backend") {
        if (index + 1 >= argc) {
          throw std::runtime_error("--backend requires a value");
        }
        backend_name = argv[++index];
        continue;
      }
      if (std::string(argv[index]) == "--level") {
        if (index + 1 >= argc) {
          throw std::runtime_error("--level requires a value");
        }
        level = devzip::parse_compression_level(argv[++index]);
        continue;
      }
      if (std::string(argv[index]) == "--verify") {
        verify_roundtrip = true;
        continue;
      }
      args.emplace_back(argv[index]);
    }

    if (args.size() < 2) {
      print_usage();
      return 1;
    }

    const std::string command = args[0];

    if (command == "compress") {
      const std::string effective_backend =
          backend_name.empty() ? default_backend_for_level(level) : backend_name;
      auto backend = make_backend_from_name(effective_backend);
      auto options = devzip::CompressionOptions::for_level(level);
      options.verify_roundtrip = options.verify_roundtrip || verify_roundtrip;
      const std::filesystem::path source = args[1];
      const std::filesystem::path archive =
          args.size() >= 3 ? std::filesystem::path(args[2])
                    : devzip::default_archive_path(source, source.parent_path());
      const auto result = devzip::create_archive(source, archive, *backend, options);
      if (json_output) {
        std::cout << "{\"status\":\"ok\",\"command\":\"compress\",\"archive\":\""
                  << json_escape(devzip::path_to_utf8(result.archive_path)) << "\"}\n";
      } else {
        std::cout << "Created " << devzip::path_to_utf8(result.archive_path) << '\n';
      }
      return 0;
    }

    if (command == "extract") {
      const std::filesystem::path archive = args[1];
      const std::filesystem::path destination =
          args.size() >= 3 ? std::filesystem::path(args[2]) : default_extract_path(archive);
      devzip::extract_archive(archive, destination);
      if (json_output) {
        std::cout << "{\"status\":\"ok\",\"command\":\"extract\",\"destination\":\""
                  << json_escape(devzip::path_to_utf8(destination)) << "\"}\n";
      } else {
        std::cout << "Extracted to " << devzip::path_to_utf8(destination) << '\n';
      }
      return 0;
    }

    if (command == "verify") {
      const std::filesystem::path archive = args[1];
      devzip::verify_archive(archive);
      if (json_output) {
        std::cout << "{\"status\":\"ok\",\"command\":\"verify\",\"archive\":\""
                  << json_escape(devzip::path_to_utf8(archive)) << "\"}\n";
      } else {
        std::cout << "Verified " << devzip::path_to_utf8(archive) << '\n';
      }
      return 0;
    }

    print_usage();
    return 1;
  } catch (const std::exception& exception) {
    if (json_output) {
      std::cerr << "{\"status\":\"error\",\"message\":\"" << json_escape(exception.what()) << "\"}\n";
    } else {
      std::cerr << exception.what() << '\n';
    }
    return 1;
  }
}
