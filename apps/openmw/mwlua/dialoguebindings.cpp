#include "dialoguebindings.hpp"
#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"
#include "apps/openmw/mwworld/store.hpp"
#include "context.hpp"
#include "object.hpp"
#include <algorithm>
#include <components/esm3/loaddial.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/vfs/pathutil.hpp>

namespace
{
    template <ESM::Dialogue::Type filter>
    class FilteredDialogueStore
    {
        const MWWorld::Store<ESM::Dialogue>& mDialogueStore;

        const ESM::Dialogue* foundDialogueFilteredOut(const ESM::Dialogue* possibleResult) const
        {
            if (possibleResult && possibleResult->mType == filter)
            {
                return possibleResult;
            }
            return nullptr;
        }

    public:
        FilteredDialogueStore()
            : mDialogueStore{ MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>() }
        {
        }

        class FilteredDialogueIterator
        {
            using DecoratedIterator = MWWorld::Store<ESM::Dialogue>::iterator;
            DecoratedIterator mIter;
            DecoratedIterator mEndIter;

        public:
            using iterator_category = DecoratedIterator::iterator_category;
            using value_type = DecoratedIterator::value_type;
            using difference_type = DecoratedIterator::difference_type;
            using pointer = DecoratedIterator::pointer;
            using reference = DecoratedIterator::reference;

            FilteredDialogueIterator(const DecoratedIterator& pointingIterator, const DecoratedIterator& end)
                : mIter{ pointingIterator }
                , mEndIter{ end }
            {
            }

            FilteredDialogueIterator& operator++()
            {
                if (mIter == mEndIter)
                {
                    return *this;
                }

                do
                {
                    ++mIter;
                } while (mIter != mEndIter && mIter->mType != filter);
                return *this;
            }

            FilteredDialogueIterator operator++(int)
            {
                FilteredDialogueIterator iter = *this;
                ++(*this);
                return iter;
            }

            FilteredDialogueIterator& operator+=(difference_type advance)
            {
                while (advance > 0 && mIter != mEndIter)
                {
                    ++(*this);
                    --advance;
                }
                return *this;
            }

            bool operator==(const FilteredDialogueIterator& x) const { return mIter == x.mIter; }

            bool operator!=(const FilteredDialogueIterator& x) const { return !(*this == x); }

            const value_type& operator*() const { return *mIter; }

            const value_type* operator->() const { return &(*mIter); }
        };

        using iterator = FilteredDialogueIterator;

        const ESM::Dialogue* search(const ESM::RefId& id) const
        {
            return foundDialogueFilteredOut(mDialogueStore.search(id));
        }

        const ESM::Dialogue* at(size_t index) const
        {
            auto result = begin();
            result += index;

            if (result == end())
            {
                return nullptr;
            }

            return &(*result);
        }

        size_t getSize() const
        {
            return std::count_if(
                mDialogueStore.begin(), mDialogueStore.end(), [](const auto& d) { return d.mType == filter; });
        }

        iterator begin() const
        {
            iterator result{ mDialogueStore.begin(), mDialogueStore.end() };
            while (result != end() && result->mType != filter)
            {
                ++result;
            }
            return result;
        }

        iterator end() const { return iterator{ mDialogueStore.end(), mDialogueStore.end() }; }
    };

    template <ESM::Dialogue::Type filter>
    void prepareBindingsForDialogueRecordStores(sol::table& table, const MWLua::Context& context)
    {
        using StoreT = FilteredDialogueStore<filter>;

        sol::state_view& lua = context.mLua->sol();
        sol::usertype<StoreT> storeBindingsClass
            = lua.new_usertype<StoreT>("ESM3_Dialogue_Type" + std::to_string(filter) + " Store");
        storeBindingsClass[sol::meta_function::to_string] = [](const StoreT& store) {
            return "{" + std::to_string(store.getSize()) + " ESM3_Dialogue_Type" + std::to_string(filter) + " records}";
        };
        storeBindingsClass[sol::meta_function::length] = [](const StoreT& store) { return store.getSize(); };
        storeBindingsClass[sol::meta_function::index] = sol::overload(
            [](const StoreT& store, size_t index) -> const ESM::Dialogue* {
                if (index == 0)
                {
                    return nullptr;
                }
                return store.at(index - 1);
            },
            [](const StoreT& store, std::string_view id) -> const ESM::Dialogue* {
                return store.search(ESM::RefId::deserializeText(id));
            });
        storeBindingsClass[sol::meta_function::ipairs] = lua["ipairsForArray"].template get<sol::function>();
        storeBindingsClass[sol::meta_function::pairs] = lua["ipairsForArray"].template get<sol::function>();

        table["records"] = StoreT{};
    }

    struct DialogueInfos
    {
        const ESM::Dialogue& parentDialogueRecord;
    };

    void prepareBindingsForDialogueRecord(sol::state_view& lua)
    {
        auto recordBindingsClass = lua.new_usertype<ESM::Dialogue>("ESM3_Dialogue");
        recordBindingsClass[sol::meta_function::to_string]
            = [](const ESM::Dialogue& rec) { return "ESM3_Dialogue[" + rec.mId.toDebugString() + "]"; };
        recordBindingsClass["id"]
            = sol::readonly_property([](const ESM::Dialogue& rec) { return rec.mId.serializeText(); });
        recordBindingsClass["name"]
            = sol::readonly_property([](const ESM::Dialogue& rec) -> std::string_view { return rec.mStringId; });
        recordBindingsClass["questName"]
            = sol::readonly_property([](const ESM::Dialogue& rec) -> sol::optional<std::string_view> {
                  if (rec.mType != ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  for (const auto& mwDialogueInfo : rec.mInfo)
                  {
                      if (mwDialogueInfo.mQuestStatus == ESM::DialInfo::QuestStatus::QS_Name)
                      {
                          return sol::optional<std::string_view>(mwDialogueInfo.mResponse);
                      }
                  }
                  return sol::nullopt;
              });
        recordBindingsClass["infos"]
            = sol::readonly_property([](const ESM::Dialogue& rec) { return DialogueInfos{ rec }; });
    }

    void prepareBindingsForDialogueRecordInfoList(sol::state_view& lua)
    {
        auto recordInfosBindingsClass = lua.new_usertype<DialogueInfos>("ESM3_Dialogue_Infos");
        recordInfosBindingsClass[sol::meta_function::to_string] = [](const DialogueInfos& store) {
            const ESM::Dialogue& dialogueRecord = store.parentDialogueRecord;
            return "{" + std::to_string(dialogueRecord.mInfo.size()) + " ESM3_Dialogue["
                + dialogueRecord.mId.toDebugString() + "] info elements}";
        };
        recordInfosBindingsClass[sol::meta_function::length]
            = [](const DialogueInfos& store) { return store.parentDialogueRecord.mInfo.size(); };
        recordInfosBindingsClass[sol::meta_function::index]
            = [](const DialogueInfos& store, size_t index) -> const ESM::DialInfo* {
            const ESM::Dialogue& dialogueRecord = store.parentDialogueRecord;
            if (index == 0 || index > dialogueRecord.mInfo.size())
            {
                return nullptr;
            }
            ESM::Dialogue::InfoContainer::const_iterator iter{ dialogueRecord.mInfo.cbegin() };
            std::advance(iter, index - 1);
            return &(*iter);
        };
        recordInfosBindingsClass[sol::meta_function::ipairs] = lua["ipairsForArray"].template get<sol::function>();
        recordInfosBindingsClass[sol::meta_function::pairs] = lua["ipairsForArray"].template get<sol::function>();
    }

    void prepareBindingsForDialogueRecordInfoListElement(sol::state_view& lua)
    {
        auto recordInfoBindingsClass = lua.new_usertype<ESM::DialInfo>("ESM3_Dialogue_Info");

        recordInfoBindingsClass[sol::meta_function::to_string]
            = [](const ESM::DialInfo& rec) { return "ESM3_Dialogue_Info[" + rec.mId.toDebugString() + "]"; };
        recordInfoBindingsClass["id"]
            = sol::readonly_property([](const ESM::DialInfo& rec) { return rec.mId.serializeText(); });
        recordInfoBindingsClass["text"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> std::string_view { return rec.mResponse; });
        recordInfoBindingsClass["questStage"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<int> {
                  if (rec.mData.mType != ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  return rec.mData.mJournalIndex;
              });
        recordInfoBindingsClass["isQuestFinished"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<bool> {
                  if (rec.mData.mType != ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  return (rec.mQuestStatus == ESM::DialInfo::QuestStatus::QS_Finished);
              });
        recordInfoBindingsClass["isQuestRestart"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<bool> {
                  if (rec.mData.mType != ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  return (rec.mQuestStatus == ESM::DialInfo::QuestStatus::QS_Restart);
              });
        recordInfoBindingsClass["isQuestName"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<bool> {
                  if (rec.mData.mType != ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  return (rec.mQuestStatus == ESM::DialInfo::QuestStatus::QS_Name);
              });
        recordInfoBindingsClass["filterActorId"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mActor.empty())
                  {
                      return sol::nullopt;
                  }
                  return rec.mActor.serializeText();
              });
        recordInfoBindingsClass["filterActorRace"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mRace.empty())
                  {
                      return sol::nullopt;
                  }
                  return rec.mRace.serializeText();
              });
        recordInfoBindingsClass["filterActorClass"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mClass.empty())
                  {
                      return sol::nullopt;
                  }
                  return rec.mClass.serializeText();
              });
        recordInfoBindingsClass["filterActorFaction"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mFaction.empty())
                  {
                      return sol::nullopt;
                  }
                  if (rec.mFactionLess)
                  {
                      return sol::optional<std::string>("");
                  }
                  return rec.mFaction.serializeText();
              });
        recordInfoBindingsClass["filterActorFactionRank"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<int> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mData.mRank == -1)
                  {
                      return sol::nullopt;
                  }
                  return rec.mData.mRank;
              });
        recordInfoBindingsClass["filterActorCell"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mCell.empty())
                  {
                      return sol::nullopt;
                  }
                  return rec.mCell.serializeText();
              });
        recordInfoBindingsClass["filterActorDisposition"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<int> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal)
                  {
                      return sol::nullopt;
                  }
                  return rec.mData.mDisposition;
              });
        recordInfoBindingsClass["filterActorGender"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<int> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mData.mGender == -1)
                  {
                      return sol::nullopt;
                  }
                  return rec.mData.mGender;
              });
        recordInfoBindingsClass["filterPlayerFaction"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mPcFaction.empty())
                  {
                      return sol::nullopt;
                  }
                  return rec.mPcFaction.serializeText();
              });
        recordInfoBindingsClass["filterPlayerFactionRank"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<int> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mData.mPCrank == -1)
                  {
                      return sol::nullopt;
                  }
                  return rec.mData.mPCrank;
              });
        recordInfoBindingsClass["sound"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string> {
                  if (rec.mData.mType == ESM::Dialogue::Type::Journal || rec.mSound.empty())
                  {
                      return sol::nullopt;
                  }
                  return Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(rec.mSound)).value();
              });
        recordInfoBindingsClass["resultScript"]
            = sol::readonly_property([](const ESM::DialInfo& rec) -> sol::optional<std::string_view> {
                  if (rec.mResultScript.empty())
                  {
                      return sol::nullopt;
                  }
                  return sol::optional<std::string_view>(rec.mResultScript);
              });
    }

    void prepareBindingsForDialogueRecords(sol::state_view& lua)
    {
        prepareBindingsForDialogueRecord(lua);
        prepareBindingsForDialogueRecordInfoList(lua);
        prepareBindingsForDialogueRecordInfoListElement(lua);
    }
}

namespace sol
{
    template <ESM::Dialogue::Type filter>
    struct is_automagical<FilteredDialogueStore<filter>> : std::false_type
    {
    };
    template <>
    struct is_automagical<ESM::Dialogue> : std::false_type
    {
    };
    template <>
    struct is_automagical<DialogueInfos> : std::false_type
    {
    };
    template <>
    struct is_automagical<ESM::DialInfo> : std::false_type
    {
    };
}

namespace MWLua
{
    sol::table initCoreDialogueBindings(const Context& context)
    {
        sol::state_view& lua = context.mLua->sol();
        sol::table api(lua, sol::create);

        sol::table journalTable(lua, sol::create);
        sol::table topicTable(lua, sol::create);
        sol::table greetingTable(lua, sol::create);
        sol::table persuasionTable(lua, sol::create);
        sol::table voiceTable(lua, sol::create);
        prepareBindingsForDialogueRecordStores<ESM::Dialogue::Type::Journal>(journalTable, context);
        prepareBindingsForDialogueRecordStores<ESM::Dialogue::Type::Topic>(topicTable, context);
        prepareBindingsForDialogueRecordStores<ESM::Dialogue::Type::Greeting>(greetingTable, context);
        prepareBindingsForDialogueRecordStores<ESM::Dialogue::Type::Persuasion>(persuasionTable, context);
        prepareBindingsForDialogueRecordStores<ESM::Dialogue::Type::Voice>(voiceTable, context);
        api["journal"] = LuaUtil::makeStrictReadOnly(journalTable);
        api["topic"] = LuaUtil::makeStrictReadOnly(topicTable);
        api["greeting"] = LuaUtil::makeStrictReadOnly(greetingTable);
        api["persuasion"] = LuaUtil::makeStrictReadOnly(persuasionTable);
        api["voice"] = LuaUtil::makeStrictReadOnly(voiceTable);

        prepareBindingsForDialogueRecords(lua);

        return LuaUtil::makeReadOnly(api);
    }
}
