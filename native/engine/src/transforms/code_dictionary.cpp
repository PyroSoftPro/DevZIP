#include "devzip/transform_pipeline.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

namespace devzip::transforms {
namespace {

constexpr std::uint8_t kMarker = 0xFF;

bool is_token_char(unsigned char value) {
  return std::isalnum(value) != 0 || value == '_';
}

std::vector<std::string> split_recipe(std::string_view recipe) {
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : recipe) {
    if (ch == '\n') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::vector<std::string> select_dictionary(std::span<const std::byte> input) {
  std::unordered_map<std::string, std::size_t> counts;
  std::string token;

  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto ch = static_cast<unsigned char>(input[index]);
    if (is_token_char(ch)) {
      token.push_back(static_cast<char>(ch));
      continue;
    }
    if (token.size() >= 4) {
      counts[token] += 1;
    }
    token.clear();
  }
  if (token.size() >= 4) {
    counts[token] += 1;
  }

  struct Candidate {
    std::string token;
    std::size_t savings;
  };

  std::vector<Candidate> candidates;
  candidates.reserve(counts.size());
  for (const auto& [name, count] : counts) {
    const auto savings = count > 1 ? count * (name.size() - 2) : 0;
    if (savings > name.size()) {
      candidates.push_back({name, savings});
    }
  }

  std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
    if (left.savings == right.savings) {
      if (left.token.size() == right.token.size()) {
        return left.token < right.token;
      }
      return left.token.size() > right.token.size();
    }
    return left.savings > right.savings;
  });

  std::vector<std::string> dictionary;
  for (const auto& candidate : candidates) {
    dictionary.push_back(candidate.token);
    if (dictionary.size() == 16) {
      break;
    }
  }
  return dictionary;
}

}  // namespace

std::vector<std::byte> apply_code_dictionary(std::span<const std::byte> input, std::string& recipe) {
  recipe.clear();
  const auto dictionary = select_dictionary(input);
  if (dictionary.empty()) {
    return std::vector<std::byte>(input.begin(), input.end());
  }

  std::vector<std::byte> output;
  output.reserve(input.size());

  std::unordered_map<std::string, std::uint8_t> indexes;
  for (std::size_t index = 0; index < dictionary.size(); ++index) {
    indexes.emplace(dictionary[index], static_cast<std::uint8_t>(index + 1));
    recipe += dictionary[index];
    recipe.push_back('\n');
  }

  for (std::size_t index = 0; index < input.size();) {
    const bool at_boundary_start =
        index == 0 || !is_token_char(static_cast<unsigned char>(input[index - 1]));

    std::string matched;
    std::uint8_t matched_index = 0;
    if (at_boundary_start) {
      for (const auto& token : dictionary) {
        if (index + token.size() > input.size()) {
          continue;
        }
        bool matches = true;
        for (std::size_t token_index = 0; token_index < token.size(); ++token_index) {
          if (static_cast<unsigned char>(input[index + token_index]) !=
              static_cast<unsigned char>(token[token_index])) {
            matches = false;
            break;
          }
        }
        const auto next_index = index + token.size();
        const bool at_boundary_end =
            next_index == input.size() || !is_token_char(static_cast<unsigned char>(input[next_index]));
        if (matches && at_boundary_end && token.size() > matched.size()) {
          matched = token;
          matched_index = indexes[token];
        }
      }
    }

    if (!matched.empty()) {
      output.push_back(static_cast<std::byte>(kMarker));
      output.push_back(static_cast<std::byte>(matched_index));
      index += matched.size();
      continue;
    }

    const auto value = static_cast<std::uint8_t>(input[index]);
    if (value == kMarker) {
      output.push_back(static_cast<std::byte>(kMarker));
      output.push_back(std::byte{0});
    } else {
      output.push_back(input[index]);
    }
    ++index;
  }

  if (output.size() >= input.size()) {
    recipe.clear();
    return std::vector<std::byte>(input.begin(), input.end());
  }

  return output;
}

std::vector<std::byte> reverse_code_dictionary(std::span<const std::byte> input, std::string_view recipe) {
  const auto dictionary = split_recipe(recipe);
  if (dictionary.empty()) {
    return std::vector<std::byte>(input.begin(), input.end());
  }

  std::vector<std::byte> output;
  output.reserve(input.size() * 2);

  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto value = static_cast<std::uint8_t>(input[index]);
    if (value != kMarker) {
      output.push_back(input[index]);
      continue;
    }

    if (index + 1 >= input.size()) {
      output.push_back(input[index]);
      continue;
    }

    const auto token_index = static_cast<std::uint8_t>(input[++index]);
    if (token_index == 0 || token_index > dictionary.size()) {
      output.push_back(static_cast<std::byte>(kMarker));
      if (token_index != 0) {
        output.push_back(static_cast<std::byte>(token_index));
      }
      continue;
    }

    for (const auto ch : dictionary[token_index - 1]) {
      output.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
  }

  return output;
}

}  // namespace devzip::transforms
