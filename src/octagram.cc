#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <rime/config.h>
#include <rime/resource.h>
#include <rime/service.h>
#include <utf8.h>

namespace rime {

struct GrammarConfig {
  int collocation_max_length = 4;
  int collocation_min_length = 3;
  double collocation_penalty = -12;
  double non_collocation_penalty = -12;
  double weak_collocation_penalty = -24;
  double rear_penalty = -18;
};

const ResourceType kGramDbType = {"gram_db", "", ".gram"};
const string kGrammarDefaultLanguage = "zh-hant";

Octagram::Octagram(Config* config, OctagramComponent* component)
    : config_(std::make_unique<GrammarConfig>()) {
  string language;
  if (config) {
    if (config->GetString("grammar/language", &language)) {
      LOG(INFO) << "use grammar: " << language;
    } else {
      return;
    }
    config->GetInt("grammar/collocation_max_length",
                  &config_->collocation_max_length);
    config->GetInt("grammar/collocation_min_length",
                  &config_->collocation_min_length);
    config->GetDouble("grammar/collocation_penalty",
                     &config_->collocation_penalty);
    config->GetDouble("grammar/non_collocation_penalty",
                     &config_->non_collocation_penalty);
    config->GetDouble("grammar/weak_collocation_penalty",
                     &config_->weak_collocation_penalty);
    config->GetDouble("grammar/rear_penalty",
                     &config_->rear_penalty);
  }
  if (!language.empty()) {
    db_ = component->GetDb(language);
  }
  string bigram_name;
  if (config && config->GetString("grammar/bigram_tsv", &bigram_name) &&
      !bigram_name.empty()) {
    const ResourceType kBigramType = {"bigram", "", ".tsv"};
    the<ResourceResolver> resolver(
        Service::instance().CreateResourceResolver(kBigramType));
    LoadBigram(resolver->ResolvePath(bigram_name));
    bigram_enabled_ = !bigram_.empty();
    config->GetDouble("grammar/bigram_penalty", &bigram_penalty_);
    if (bigram_enabled_) {
      DLOG(INFO) << "bigram augmentation enabled: " << bigram_name;
    }
  }
}

Octagram::~Octagram() {}

bool Octagram::is_loaded() const { return db_ != nullptr || bigram_enabled_; }

inline static double scale_value(int value) {
  return value >= 0 ? double(value) / GramDb::kValueScale : -1;
}

inline static bool update_result(double& result, double new_value) {
  if (new_value > result) {
    result = new_value;
    return true;
  }
  return false;
}

inline static const char* str_begin(const string& str) {
  return str.c_str();
}

inline static const char* str_end(const string& str) {
  return str.c_str() + str.length();
}

inline static const char* last_n_unicode(const string& str,
                                         int max,
                                         int& out_count) {
  const char* begin = str_begin(str);
  const char* p = str_end(str);
  out_count = 0;
  while (p != begin && out_count < max) {
    utf8::unchecked::prior(p);
    ++out_count;
  }
  return p;
}

inline static const char* first_n_unicode(const string& str,
                                          int max,
                                          int& out_count) {
  const char* p = str_begin(str);
  const char* end = str_end(str);
  out_count = 0;
  while (p != end && out_count < max) {
    utf8::unchecked::next(p);
    ++out_count;
  }
  return p;
}

inline static bool matches_whole_query(const char* context_ptr,
                                       const string& context_query,
                                       size_t match_length,
                                       const string& word_query) {
  return context_ptr == str_begin(context_query) &&
      match_length == word_query.length();
}

double Octagram::Query(const string& context,
                       const string& word,
                       bool is_rear) {
  if (!db_ || context.empty()) {
    return config_->non_collocation_penalty;
  }
  double result = config_->non_collocation_penalty;
  bool gram_hit = false;
  GramDb::Match matches[GramDb::kMaxResults];
  int n = (std::min)(grammar::kMaxEncodedUnicode,
                     config_->collocation_max_length - 1);
  int context_len = 0;
  string context_query = grammar::encode(
      last_n_unicode(context, n, context_len),
      str_end(context));
  int word_query_len = 0;
  string word_query = grammar::encode(
      str_begin(word),
      first_n_unicode(word, n, word_query_len));
  for (const char* context_ptr = str_begin(context_query);
       context_len > 0;
       --context_len, context_ptr = grammar::next_unicode(context_ptr)) {
    int num_results = db_->Lookup(context_ptr, word_query, matches);
    if (num_results > 0) {
      gram_hit = true;
    }
    DLOG(INFO) << "Lookup(" << context_ptr << " + " << word_query << ") returns "
               << num_results << " results";
    for (auto i = 0; i < num_results; ++i) {
      const auto& match(matches[i]);
      const int match_len = grammar::unicode_length(word_query, match.length);
      DLOG(INFO) << "match[" << match.length << "] = "
                 << scale_value(match.value);
      const int collocation_len = context_len + match_len;
      if (update_result(result,
                        scale_value(match.value) +
                        (collocation_len >= config_->collocation_min_length ||
                         matches_whole_query(context_ptr, context_query,
                                             match.length, word_query)
                         ? config_->collocation_penalty
                         : config_->weak_collocation_penalty))) {
        DLOG(INFO) << "update: " << context << "[" << context_len << "] + "
                   << word << "[" << match_len << "] = " << result;
      }
    }
  }
  if (is_rear) {
    int word_len = utf8::unchecked::distance(word.c_str(),
                                             word.c_str() + word.length());
    if (word_query_len == word_len &&
        db_->Lookup(word_query, "$", matches) > 0 &&
        update_result(result,
                      scale_value(matches[0].value) + config_->rear_penalty)) {
      DLOG(INFO) << "update: " << word << "$ / " << result;
    }
  }
  // 门控：essay 无覆盖时才用口语 bigram（独立罚分，可配置放大）。
  DLOG(INFO) << "query gate: ctx=" << context << " word=" << word
             << " gram_hit=" << gram_hit << " bg_on=" << bigram_enabled_
             << " result=" << result;
  if (bigram_enabled_ && !gram_hit) {
    double bg = QueryBigram(context, word);
    if (bg > result) {
      result = bg;
    }
  }
  DLOG(INFO) << "context = " << context << ", word = " << word
             << " / " << result;
  return result;
}

namespace {
// 原始 UTF-8 单个字符的字节长度（context 是明文，不经过 gram 键编码）。
inline size_t raw_utf8_char_len(unsigned char b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 1;
}
}  // namespace

double Octagram::QueryBigram(const string& context, const string& word) {
  if (context.empty()) {
    return config_->non_collocation_penalty;
  }
  // 上下文末尾 ≤4 字，最长后缀优先。用原始 UTF-8 字符边界收集
  // 最近 5 个起点，取其中最近 ≤4 个作为后缀起点。
  size_t ring[5];
  int rc = 0;
  for (size_t i = 0; i < context.size(); ) {
    ring[rc % 5] = i;
    ++rc;
    i += raw_utf8_char_len(static_cast<unsigned char>(context[i]));
    if (i > context.size()) {
      break;  // 容错：截断的多字节序列按已收集的边界处理
    }
  }
  int total = rc;
  int k = total < 4 ? total : 4;
  for (int idx = 0; idx < k; ++idx) {
    int slot = (rc - 1 - idx) % 5;
    size_t start = ring[slot];
    auto it = bigram_.find(context.substr(start));
    if (it == bigram_.end()) {
      continue;
    }
    for (const auto& kw : it->second) {
      if (kw.first == word) {
        // 独立罚分（默认 -4）：口语对典型 ln(count)≈9-11，净分 +5~7，
        // 足以翻转"紧凑词碾压虚词组合"的排序。
        double score = std::log(static_cast<double>(kw.second)) + bigram_penalty_;
        DLOG(INFO) << "bigram hit: " << context << " + " << word
                   << " count=" << kw.second << " score=" << score;
        return score;
      }
    }
    break;  // 只用最长命中头
  }
  return config_->non_collocation_penalty;
}

void Octagram::LoadBigram(const path& tsv_path) {
  std::ifstream fin(tsv_path.string());
  if (!fin) {
    LOG(ERROR) << "bigram tsv not found: " << tsv_path.string();
    return;
  }
  string line;
  while (std::getline(fin, line)) {
    if (line.compare(0, 2, "n	") != 0) {
      continue;
    }
    size_t t1 = line.find('	', 2);
    if (t1 == string::npos) {
      continue;
    }
    string head = line.substr(2, t1 - 2);
    auto& row = bigram_[head];
    size_t pos = t1 + 1;
    while (pos < line.size()) {
      size_t sep = line.find(' ', pos);
      if (sep == string::npos) {
        sep = line.size();
      }
      size_t colon = line.rfind(':', sep);
      if (colon != string::npos && colon >= pos) {
        int count = atoi(line.substr(colon + 1, sep - colon - 1).c_str());
        if (count > 0) {
          string word = line.substr(pos, colon - pos);
          if (!word.empty()) {
            row.push_back({word, count});
          }
        }
      }
      pos = sep + 1;
    }
  }
  DLOG(INFO) << "bigram heads loaded: " << bigram_.size();
}

OctagramComponent::OctagramComponent() {}

OctagramComponent::~OctagramComponent() {}

Octagram* OctagramComponent::Create(Config* config) {
  return new Octagram(config, this);
}

GramDb* OctagramComponent::GetDb(const string& language) {
  auto& loaded = db_by_language_[language];
  if (!loaded) {
    the<ResourceResolver> resolver(
        Service::instance().CreateResourceResolver(kGramDbType));
    the<GramDb> db =
        std::make_unique<GramDb>(resolver->ResolvePath(language));
    if (!db->Load()) {
      LOG(ERROR) << "failed to load grammar database: " << language;
      return nullptr;
    }
    loaded = std::move(db);
  }
  return loaded.get();
}

}  // namespace rime
