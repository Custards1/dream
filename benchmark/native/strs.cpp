// strs.dr in C++: 200,000 numbers as words, joined, split again, upper-cased and searched.
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> words;
    for (int i = 0; i < 200000; ++i) words.push_back(std::to_string(i));
    std::string joined;
    for (size_t i = 0; i < words.size(); ++i) { if (i) joined += ' '; joined += words[i]; }
    std::vector<std::string> parts;
    size_t from = 0;
    for (;;) {
        size_t at = joined.find(' ', from);
        parts.push_back(joined.substr(from, at == std::string::npos ? std::string::npos : at - from));
        if (at == std::string::npos) break;
        from = at + 1;
    }
    long n = 0;
    for (const std::string& w : parts) {
        std::string up = w;
        for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
        n += long(up.size()) + (w.find("99") != std::string::npos ? 1 : 0);
    }
    std::printf("[%zu, %ld]\n", joined.size(), n);
}
