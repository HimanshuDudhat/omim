#pragma once
#include "intermediate_result.hpp"
#include "lang_keywords_scorer.hpp"

#include "../indexer/search_trie.hpp"
#include "../indexer/index.hpp"   // for Index::MwmLock

#include "../geometry/rect2d.hpp"

#include "../base/buffer_vector.hpp"
#include "../base/limited_priority_queue.hpp"
#include "../base/string_utils.hpp"

#include "../std/map.hpp"
#include "../std/scoped_ptr.hpp"
#include "../std/string.hpp"
#include "../std/unordered_set.hpp"
#include "../std/vector.hpp"


class FeatureType;
class CategoriesHolder;

namespace storage { class CountryInfoGetter; }

namespace search
{

namespace impl
{
  class FeatureLoader;
  class BestNameFinder;
  class PreResult2Maker;
  struct Locality;
  class DoFindLocality;
}

class Query
{
public:
  static int const SCALE_SEARCH_DEPTH = 7;
  static int const ADDRESS_SCALE = 10;

  struct SuggestT
  {
    strings::UniString m_name;
    uint8_t m_prefixLength;
    int8_t m_lang;

    SuggestT(strings::UniString const & name, uint8_t len, int8_t lang)
      : m_name(name), m_prefixLength(len), m_lang(lang)
    {
    }
  };

  // Vector of suggests.
  typedef vector<SuggestT> StringsToSuggestVectorT;

  Query(Index const * pIndex,
        CategoriesHolder const * pCategories,
        StringsToSuggestVectorT const * pStringsToSuggest,
        storage::CountryInfoGetter const * pInfoGetter,
        size_t resultsNeeded = 10);
  ~Query();

  void SetViewport(m2::RectD viewport[], size_t count);

  static const int empty_pos_value = -1000;
  inline void SetPosition(m2::PointD const & pos) { m_position = pos; }
  inline void NullPosition() { m_position = m2::PointD(empty_pos_value, empty_pos_value); }

  inline void SetSearchInWorld(bool b) { m_worldSearch = b; }

  void SetPreferredLanguage(string const & lang);
  void SetInputLanguage(int8_t lang);

  void Search(string const & query, Results & res);
  void SearchAllInViewport(m2::RectD const & viewport, Results & res, unsigned int resultsNeeded = 30);
  void SearchAdditional(Results & res);

  void ClearCache();

  inline void DoCancel() { m_cancel = true; }
  inline bool IsCanceled() const { return m_cancel; }
  struct CancelException {};

  typedef trie::ValueReader::ValueType TrieValueT;

private:
  friend class impl::FeatureLoader;
  friend class impl::BestNameFinder;
  friend class impl::PreResult2Maker;
  friend class impl::DoFindLocality;

  void InitSearch(string const & query);
  void ClearQueues();

  typedef vector<MwmInfo> MWMVectorT;
  typedef vector<vector<uint32_t> > OffsetsVectorT;

  void SetViewportByIndex(MWMVectorT const & mwmInfo, m2::RectD const & viewport, size_t idx);
  void UpdateViewportOffsets(MWMVectorT const & mwmInfo, m2::RectD const & rect,
                             OffsetsVectorT & offsets);
  void ClearCache(size_t ind);

  void AddResultFromTrie(TrieValueT const & val, size_t mwmID, int viewportID);

  void FlushResults(Results & res, void (Results::*pAddFn)(Result const &));

  struct Params
  {
    typedef vector<strings::UniString> TokensVectorT;
    typedef unordered_set<int8_t> LangsSetT;

    vector<TokensVectorT> m_tokens;
    TokensVectorT m_prefixTokens;
    LangsSetT m_langs;

    /// Initialize search params (tokens, languages).
    /// @param[in]  isLocalities  Use true when search for locality in World.
    Params(Query const & q, bool isLocalities = false);

    /// @param[in] eraseInds Sorted vector of token's indexes.
    void EraseTokens(vector<size_t> const & eraseInds);

    bool IsEmpty() const { return (m_tokens.empty() && m_prefixTokens.empty()); }
    bool IsLangExist(uint8_t l) const { return (m_langs.count(l) > 0); }

  private:
    void AddSynonims(Query const & q, bool isLocalities);
    void FillLanguages(Query const & q);
  };

  void SearchAddress();
  bool SearchLocality(MwmValue * pMwm, impl::Locality & res);

  void SearchFeatures();
  void SearchFeatures(Params const & params, MWMVectorT const & mwmInfo, int ind);

  /// Do search in particular map. Pass offsets == 0 if you don't want
  /// results set to be intersected with source feature's offsets.
  void SearchInMWM(Index::MwmLock const & mwmLock, Params const & params, int ind);

  void SuggestStrings(Results & res);
  bool MatchForSuggestionsImpl(strings::UniString const & token, int8_t lang, Results & res);
  void MatchForSuggestions(strings::UniString const & token, Results & res);

  void GetBestMatchName(FeatureType const & f, uint32_t & penalty, string & name) const;

  Result MakeResult(impl::PreResult2 const & r, set<uint32_t> const * pPrefferedTypes = 0) const;

  Index const * m_pIndex;
  CategoriesHolder const * m_pCategories;
  StringsToSuggestVectorT const * m_pStringsToSuggest;
  storage::CountryInfoGetter const * m_pInfoGetter;

  volatile bool m_cancel;

  buffer_vector<strings::UniString, 32> m_tokens;
  strings::UniString m_prefix;

  /// 0 - current viewport rect
  /// 1 - near me rect
  /// 2 - around city rect
  static size_t const RECTSCOUNT = 3;
  static int const ADDRESS_RECT_ID = RECTSCOUNT-1;

  m2::RectD m_viewport[RECTSCOUNT];
  bool m_worldSearch;

  /// @name Get ranking params.
  /// @param[in]  viewportID  Index of search viewport (@see comments above); -1 means default viewport.
  //@{
  /// @return Rect for viewport-distance calculation.
  m2::RectD const & GetViewport(int viewportID = -1) const;
  m2::PointD GetPosition(int viewportID = -1) const;
  //@}

  m2::PointD m_position;

  void SetLanguage(int id, int8_t lang);
  int8_t GetLanguage(int id) const;

  LangKeywordsScorer m_keywordsScorer;

  OffsetsVectorT m_offsetsInViewport[RECTSCOUNT];

  template <class ParamT, class RefT> class CompareT
  {
    typedef bool (*FunctionT) (ParamT const &, ParamT const &);
    FunctionT m_fn;

  public:
    CompareT() : m_fn(0) {}
    explicit CompareT(FunctionT const & fn) : m_fn(fn) {}

    template <class T> bool operator() (T const & v1, T const & v2) const
    {
      RefT getR;
      return m_fn(getR(v1), getR(v2));
    }
  };

  struct NothingRef
  {
    template <class T> T const & operator() (T const & t) const { return t; }
  };
  struct RefPointer
  {
    template <class T> typename T::value_type const & operator() (T const & t) const { return *t; }
    template <class T> T const & operator() (T const * t) const { return *t; }
  };

  typedef CompareT<impl::PreResult1, NothingRef> QueueCompareT;
  typedef my::limited_priority_queue<impl::PreResult1, QueueCompareT> QueueT;

public:
  static const size_t m_qCount = 3;

private:
  QueueT m_results[m_qCount];
};

}  // namespace search
