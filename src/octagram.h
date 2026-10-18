#ifndef RIME_OCTAGRAM_H_
#define RIME_OCTAGRAM_H_

#include <rime/common.h>
#include <rime/component.h>
#include <rime/resource.h>
#include <rime/gear/grammar.h>
#include <unordered_map>

namespace rime {

extern const ResourceType kGramDbType;
extern const string kGrammarDefaultLanguage;

class Config;
class GramDb;
struct GrammarConfig;
class OctagramComponent;

class Octagram : public Grammar {
 public:
  Octagram(Config* config, OctagramComponent* component);
  virtual ~Octagram();
  double Query(const string& context,
               const string& word,
               bool is_rear) override;
  bool is_loaded() const override;

 private:
  // 口语 bigram 增强：grammar/bigram_tsv 配置后启用，
  // Query 取 .gram 与 bigram 两源的最大分（书面+口语互补）。
  double QueryBigram(const string& context, const string& word);
  void LoadBigram(const path& tsv_path);

  the<GrammarConfig> config_;
  GramDb* db_ = nullptr;
  bool bigram_enabled_ = false;
  double bigram_penalty_ = -4.0;
  std::unordered_map<string, vector<pair<string, int>>> bigram_;
};

class OctagramComponent : public Grammar::Component {
 public:
  OctagramComponent();
  virtual ~OctagramComponent();

  Octagram* Create(Config* config) override;

  GramDb* GetDb(const string& language);

 private:
  map<string, the<GramDb>> db_by_language_;
};

}  // namespace rime

#endif  // RIME_OCTAGRAM_H_
