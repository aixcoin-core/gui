// Copyright (c) 2022-present The Aixcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define AIXCOINKERNEL_BUILD

#include <kernel/aixcoinkernel.h>

#include <chain.h>
#include <coins.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/chainparams.h>
#include <kernel/checks.h>
#include <kernel/context.h>
#include <kernel/notifications_interface.h>
#include <kernel/warning.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/task_runner.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using kernel::ChainstateRole;
using util::ImmediateTaskRunner;

// Define G_TRANSLATION_FUN symbol in libaixcoinkernel library so users of the
// library aren't required to export this symbol
extern const TranslateFn G_TRANSLATION_FUN{nullptr};

static const kernel::Context aixk_context_static{};

namespace {

bool is_valid_flag_combination(script_verify_flags flags)
{
    if (flags & SCRIPT_VERIFY_CLEANSTACK && ~flags & (SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS)) return false;
    if (flags & SCRIPT_VERIFY_WITNESS && ~flags & SCRIPT_VERIFY_P2SH) return false;
    return true;
}

class WriterStream
{
private:
    aixk_WriteBytes m_writer;
    void* m_user_data;

public:
    WriterStream(aixk_WriteBytes writer, void* user_data)
        : m_writer{writer}, m_user_data{user_data} {}

    //
    // Stream subset
    //
    void write(std::span<const std::byte> src)
    {
        if (m_writer(src.data(), src.size(), m_user_data) != 0) {
            throw std::runtime_error("Failed to write serialization data");
        }
    }

    template <typename T>
    WriterStream& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return *this;
    }
};

template <typename C, typename CPP>
struct Handle {
    static C* ref(CPP* cpp_type)
    {
        return reinterpret_cast<C*>(cpp_type);
    }

    static const C* ref(const CPP* cpp_type)
    {
        return reinterpret_cast<const C*>(cpp_type);
    }

    template <typename... Args>
    static C* create(Args&&... args)
    {
        auto cpp_obj{std::make_unique<CPP>(std::forward<Args>(args)...)};
        return ref(cpp_obj.release());
    }

    static C* copy(const C* ptr)
    {
        auto cpp_obj{std::make_unique<CPP>(get(ptr))};
        return ref(cpp_obj.release());
    }

    static const CPP& get(const C* ptr)
    {
        return *reinterpret_cast<const CPP*>(ptr);
    }

    static CPP& get(C* ptr)
    {
        return *reinterpret_cast<CPP*>(ptr);
    }

    static void operator delete(void* ptr)
    {
        delete reinterpret_cast<CPP*>(ptr);
    }
};

} // namespace

struct aixk_BlockTreeEntry: Handle<aixk_BlockTreeEntry, CBlockIndex> {};
struct aixk_Block : Handle<aixk_Block, std::shared_ptr<const CBlock>> {};
struct aixk_BlockValidationState : Handle<aixk_BlockValidationState, BlockValidationState> {};

namespace {

BCLog::Level get_bclog_level(aixk_LogLevel level)
{
    switch (level) {
    case aixk_LogLevel_INFO: {
        return BCLog::Level::Info;
    }
    case aixk_LogLevel_DEBUG: {
        return BCLog::Level::Debug;
    }
    case aixk_LogLevel_TRACE: {
        return BCLog::Level::Trace;
    }
    }
    assert(false);
}

BCLog::LogFlags get_bclog_flag(aixk_LogCategory category)
{
    switch (category) {
    case aixk_LogCategory_BENCH: {
        return BCLog::LogFlags::BENCH;
    }
    case aixk_LogCategory_BLOCKSTORAGE: {
        return BCLog::LogFlags::BLOCKSTORAGE;
    }
    case aixk_LogCategory_COINDB: {
        return BCLog::LogFlags::COINDB;
    }
    case aixk_LogCategory_LEVELDB: {
        return BCLog::LogFlags::LEVELDB;
    }
    case aixk_LogCategory_MEMPOOL: {
        return BCLog::LogFlags::MEMPOOL;
    }
    case aixk_LogCategory_PRUNE: {
        return BCLog::LogFlags::PRUNE;
    }
    case aixk_LogCategory_RAND: {
        return BCLog::LogFlags::RAND;
    }
    case aixk_LogCategory_REINDEX: {
        return BCLog::LogFlags::REINDEX;
    }
    case aixk_LogCategory_VALIDATION: {
        return BCLog::LogFlags::VALIDATION;
    }
    case aixk_LogCategory_KERNEL: {
        return BCLog::LogFlags::KERNEL;
    }
    case aixk_LogCategory_ALL: {
        return BCLog::LogFlags::ALL;
    }
    }
    assert(false);
}

aixk_SynchronizationState cast_state(SynchronizationState state)
{
    switch (state) {
    case SynchronizationState::INIT_REINDEX:
        return aixk_SynchronizationState_INIT_REINDEX;
    case SynchronizationState::INIT_DOWNLOAD:
        return aixk_SynchronizationState_INIT_DOWNLOAD;
    case SynchronizationState::POST_INIT:
        return aixk_SynchronizationState_POST_INIT;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

aixk_Warning cast_aixk_warning(kernel::Warning warning)
{
    switch (warning) {
    case kernel::Warning::UNKNOWN_NEW_RULES_ACTIVATED:
        return aixk_Warning_UNKNOWN_NEW_RULES_ACTIVATED;
    case kernel::Warning::LARGE_WORK_INVALID_CHAIN:
        return aixk_Warning_LARGE_WORK_INVALID_CHAIN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

struct LoggingConnection {
    std::unique_ptr<std::list<std::function<void(const std::string&)>>::iterator> m_connection;
    void* m_user_data;
    std::function<void(void* user_data)> m_deleter;

    LoggingConnection(aixk_LogCallback callback, void* user_data, aixk_DestroyCallback user_data_destroy_callback)
    {
        LOCK(cs_main);

        auto connection{LogInstance().PushBackCallback([callback, user_data](const std::string& str) { callback(user_data, str.c_str(), str.length()); })};

        // Only start logging if we just added the connection.
        if (LogInstance().NumConnections() == 1 && !LogInstance().StartLogging()) {
            LogError("Logger start failed.");
            LogInstance().DeleteCallback(connection);
            if (user_data && user_data_destroy_callback) {
                user_data_destroy_callback(user_data);
            }
            throw std::runtime_error("Failed to start logging");
        }

        m_connection = std::make_unique<std::list<std::function<void(const std::string&)>>::iterator>(connection);
        m_user_data = user_data;
        m_deleter = user_data_destroy_callback;

        LogDebug(BCLog::KERNEL, "Logger connected.");
    }

    ~LoggingConnection()
    {
        LOCK(cs_main);
        LogDebug(BCLog::KERNEL, "Logger disconnecting.");

        // Switch back to buffering by calling DisconnectTestLogger if the
        // connection that we are about to remove is the last one.
        if (LogInstance().NumConnections() == 1) {
            LogInstance().DisconnectTestLogger();
        } else {
            LogInstance().DeleteCallback(*m_connection);
        }

        m_connection.reset();
        if (m_user_data && m_deleter) {
            m_deleter(m_user_data);
        }
    }
};

class KernelNotifications final : public kernel::Notifications
{
private:
    aixk_NotificationInterfaceCallbacks m_cbs;

public:
    KernelNotifications(aixk_NotificationInterfaceCallbacks cbs)
        : m_cbs{cbs}
    {
    }

    ~KernelNotifications()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data_destroy = nullptr;
        m_cbs.user_data = nullptr;
    }

    kernel::InterruptResult blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress) override
    {
        if (m_cbs.block_tip) m_cbs.block_tip(m_cbs.user_data, cast_state(state), aixk_BlockTreeEntry::ref(&index), verification_progress);
        return {};
    }
    void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override
    {
        if (m_cbs.header_tip) m_cbs.header_tip(m_cbs.user_data, cast_state(state), height, timestamp, presync ? 1 : 0);
    }
    void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override
    {
        if (m_cbs.progress) m_cbs.progress(m_cbs.user_data, title.original.c_str(), title.original.length(), progress_percent, resume_possible ? 1 : 0);
    }
    void warningSet(kernel::Warning id, const bilingual_str& message) override
    {
        if (m_cbs.warning_set) m_cbs.warning_set(m_cbs.user_data, cast_aixk_warning(id), message.original.c_str(), message.original.length());
    }
    void warningUnset(kernel::Warning id) override
    {
        if (m_cbs.warning_unset) m_cbs.warning_unset(m_cbs.user_data, cast_aixk_warning(id));
    }
    void flushError(const bilingual_str& message) override
    {
        if (m_cbs.flush_error) m_cbs.flush_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
    void fatalError(const bilingual_str& message) override
    {
        if (m_cbs.fatal_error) m_cbs.fatal_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
};

class KernelValidationInterface final : public CValidationInterface
{
public:
    aixk_ValidationInterfaceCallbacks m_cbs;

    explicit KernelValidationInterface(const aixk_ValidationInterfaceCallbacks vi_cbs) : m_cbs{vi_cbs} {}

    ~KernelValidationInterface()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data = nullptr;
        m_cbs.user_data_destroy = nullptr;
    }

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& stateIn) override
    {
        if (m_cbs.block_checked) {
            m_cbs.block_checked(m_cbs.user_data,
                                aixk_Block::copy(aixk_Block::ref(&block)),
                                aixk_BlockValidationState::ref(&stateIn));
        }
    }

    void NewPoWValidBlock(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block) override
    {
        if (m_cbs.pow_valid_block) {
            m_cbs.pow_valid_block(m_cbs.user_data,
                                  aixk_Block::copy(aixk_Block::ref(&block)),
                                  aixk_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_connected) {
            m_cbs.block_connected(m_cbs.user_data,
                                  aixk_Block::copy(aixk_Block::ref(&block)),
                                  aixk_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_disconnected) {
            m_cbs.block_disconnected(m_cbs.user_data,
                                     aixk_Block::copy(aixk_Block::ref(&block)),
                                     aixk_BlockTreeEntry::ref(pindex));
        }
    }
};

struct ContextOptions {
    mutable Mutex m_mutex;
    std::unique_ptr<const CChainParams> m_chainparams GUARDED_BY(m_mutex);
    std::shared_ptr<KernelNotifications> m_notifications GUARDED_BY(m_mutex);
    std::shared_ptr<KernelValidationInterface> m_validation_interface GUARDED_BY(m_mutex);
};

class Context
{
public:
    std::unique_ptr<kernel::Context> m_context;

    std::shared_ptr<KernelNotifications> m_notifications;

    std::unique_ptr<util::SignalInterrupt> m_interrupt;

    std::unique_ptr<ValidationSignals> m_signals;

    std::unique_ptr<const CChainParams> m_chainparams;

    std::shared_ptr<KernelValidationInterface> m_validation_interface;

    Context(const ContextOptions* options, bool& sane)
        : m_context{std::make_unique<kernel::Context>()},
          m_interrupt{std::make_unique<util::SignalInterrupt>()}
    {
        if (options) {
            LOCK(options->m_mutex);
            if (options->m_chainparams) {
                m_chainparams = std::make_unique<const CChainParams>(*options->m_chainparams);
            }
            if (options->m_notifications) {
                m_notifications = options->m_notifications;
            }
            if (options->m_validation_interface) {
                m_signals = std::make_unique<ValidationSignals>(std::make_unique<ImmediateTaskRunner>());
                m_validation_interface = options->m_validation_interface;
                m_signals->RegisterSharedValidationInterface(m_validation_interface);
            }
        }

        if (!m_chainparams) {
            m_chainparams = CChainParams::Main();
        }
        if (!m_notifications) {
            m_notifications = std::make_shared<KernelNotifications>(aixk_NotificationInterfaceCallbacks{
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
        }

        if (!kernel::SanityChecks(*m_context)) {
            sane = false;
        }
    }

    ~Context()
    {
        if (m_signals) {
            m_signals->UnregisterSharedValidationInterface(m_validation_interface);
        }
    }
};

//! Helper struct to wrap the ChainstateManager-related Options
struct ChainstateManagerOptions {
    mutable Mutex m_mutex;
    ChainstateManager::Options m_chainman_options GUARDED_BY(m_mutex);
    node::BlockManager::Options m_blockman_options GUARDED_BY(m_mutex);
    std::shared_ptr<const Context> m_context;
    node::ChainstateLoadOptions m_chainstate_load_options GUARDED_BY(m_mutex);

    ChainstateManagerOptions(const std::shared_ptr<const Context>& context, const fs::path& data_dir, const fs::path& blocks_dir)
        : m_chainman_options{ChainstateManager::Options{
              .chainparams = *context->m_chainparams,
              .datadir = data_dir,
              .notifications = *context->m_notifications,
              .signals = context->m_signals.get()}},
          m_blockman_options{node::BlockManager::Options{
              .chainparams = *context->m_chainparams,
              .blocks_dir = blocks_dir,
              .notifications = *context->m_notifications,
              .block_tree_db_params = DBParams{
                  .path = data_dir / "blocks" / "index",
                  .cache_bytes = kernel::CacheSizes{DEFAULT_KERNEL_CACHE}.block_tree_db,
              }}},
          m_context{context}, m_chainstate_load_options{node::ChainstateLoadOptions{}}
    {
    }
};

struct ChainMan {
    std::unique_ptr<ChainstateManager> m_chainman;
    std::shared_ptr<const Context> m_context;

    ChainMan(std::unique_ptr<ChainstateManager> chainman, std::shared_ptr<const Context> context)
        : m_chainman(std::move(chainman)), m_context(std::move(context)) {}
};

} // namespace

struct aixk_Transaction : Handle<aixk_Transaction, std::shared_ptr<const CTransaction>> {};
struct aixk_TransactionOutput : Handle<aixk_TransactionOutput, CTxOut> {};
struct aixk_ScriptPubkey : Handle<aixk_ScriptPubkey, CScript> {};
struct aixk_LoggingConnection : Handle<aixk_LoggingConnection, LoggingConnection> {};
struct aixk_ContextOptions : Handle<aixk_ContextOptions, ContextOptions> {};
struct aixk_Context : Handle<aixk_Context, std::shared_ptr<const Context>> {};
struct aixk_ChainParameters : Handle<aixk_ChainParameters, CChainParams> {};
struct aixk_ChainstateManagerOptions : Handle<aixk_ChainstateManagerOptions, ChainstateManagerOptions> {};
struct aixk_ChainstateManager : Handle<aixk_ChainstateManager, ChainMan> {};
struct aixk_Chain : Handle<aixk_Chain, CChain> {};
struct aixk_BlockSpentOutputs : Handle<aixk_BlockSpentOutputs, std::shared_ptr<CBlockUndo>> {};
struct aixk_TransactionSpentOutputs : Handle<aixk_TransactionSpentOutputs, CTxUndo> {};
struct aixk_Coin : Handle<aixk_Coin, Coin> {};
struct aixk_BlockHash : Handle<aixk_BlockHash, uint256> {};
struct aixk_TransactionInput : Handle<aixk_TransactionInput, CTxIn> {};
struct aixk_TransactionOutPoint: Handle<aixk_TransactionOutPoint, COutPoint> {};
struct aixk_Txid: Handle<aixk_Txid, Txid> {};
struct aixk_PrecomputedTransactionData : Handle<aixk_PrecomputedTransactionData, PrecomputedTransactionData> {};
struct aixk_BlockHeader: Handle<aixk_BlockHeader, CBlockHeader> {};

aixk_Transaction* aixk_transaction_create(const void* raw_transaction, size_t raw_transaction_len)
{
    if (raw_transaction == nullptr && raw_transaction_len != 0) {
        return nullptr;
    }
    try {
        SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_transaction), raw_transaction_len}};
        return aixk_Transaction::create(std::make_shared<const CTransaction>(deserialize, TX_WITH_WITNESS, stream));
    } catch (...) {
        return nullptr;
    }
}

size_t aixk_transaction_count_outputs(const aixk_Transaction* transaction)
{
    return aixk_Transaction::get(transaction)->vout.size();
}

const aixk_TransactionOutput* aixk_transaction_get_output_at(const aixk_Transaction* transaction, size_t output_index)
{
    const CTransaction& tx = *aixk_Transaction::get(transaction);
    assert(output_index < tx.vout.size());
    return aixk_TransactionOutput::ref(&tx.vout[output_index]);
}

size_t aixk_transaction_count_inputs(const aixk_Transaction* transaction)
{
    return aixk_Transaction::get(transaction)->vin.size();
}

const aixk_TransactionInput* aixk_transaction_get_input_at(const aixk_Transaction* transaction, size_t input_index)
{
    assert(input_index < aixk_Transaction::get(transaction)->vin.size());
    return aixk_TransactionInput::ref(&aixk_Transaction::get(transaction)->vin[input_index]);
}

const aixk_Txid* aixk_transaction_get_txid(const aixk_Transaction* transaction)
{
    return aixk_Txid::ref(&aixk_Transaction::get(transaction)->GetHash());
}

aixk_Transaction* aixk_transaction_copy(const aixk_Transaction* transaction)
{
    return aixk_Transaction::copy(transaction);
}

int aixk_transaction_to_bytes(const aixk_Transaction* transaction, aixk_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(aixk_Transaction::get(transaction));
        return 0;
    } catch (...) {
        return -1;
    }
}

void aixk_transaction_destroy(aixk_Transaction* transaction)
{
    delete transaction;
}

aixk_ScriptPubkey* aixk_script_pubkey_create(const void* script_pubkey, size_t script_pubkey_len)
{
    if (script_pubkey == nullptr && script_pubkey_len != 0) {
        return nullptr;
    }
    auto data = std::span{reinterpret_cast<const uint8_t*>(script_pubkey), script_pubkey_len};
    return aixk_ScriptPubkey::create(data.begin(), data.end());
}

int aixk_script_pubkey_to_bytes(const aixk_ScriptPubkey* script_pubkey_, aixk_WriteBytes writer, void* user_data)
{
    const auto& script_pubkey{aixk_ScriptPubkey::get(script_pubkey_)};
    return writer(script_pubkey.data(), script_pubkey.size(), user_data);
}

aixk_ScriptPubkey* aixk_script_pubkey_copy(const aixk_ScriptPubkey* script_pubkey)
{
    return aixk_ScriptPubkey::copy(script_pubkey);
}

void aixk_script_pubkey_destroy(aixk_ScriptPubkey* script_pubkey)
{
    delete script_pubkey;
}

aixk_TransactionOutput* aixk_transaction_output_create(const aixk_ScriptPubkey* script_pubkey, int64_t amount)
{
    return aixk_TransactionOutput::create(amount, aixk_ScriptPubkey::get(script_pubkey));
}

aixk_TransactionOutput* aixk_transaction_output_copy(const aixk_TransactionOutput* output)
{
    return aixk_TransactionOutput::copy(output);
}

const aixk_ScriptPubkey* aixk_transaction_output_get_script_pubkey(const aixk_TransactionOutput* output)
{
    return aixk_ScriptPubkey::ref(&aixk_TransactionOutput::get(output).scriptPubKey);
}

int64_t aixk_transaction_output_get_amount(const aixk_TransactionOutput* output)
{
    return aixk_TransactionOutput::get(output).nValue;
}

void aixk_transaction_output_destroy(aixk_TransactionOutput* output)
{
    delete output;
}

aixk_PrecomputedTransactionData* aixk_precomputed_transaction_data_create(
    const aixk_Transaction* tx_to,
    const aixk_TransactionOutput** spent_outputs_, size_t spent_outputs_len)
{
    try {
        const CTransaction& tx{*aixk_Transaction::get(tx_to)};
        auto txdata{aixk_PrecomputedTransactionData::create()};
        if (spent_outputs_ != nullptr && spent_outputs_len > 0) {
            assert(spent_outputs_len == tx.vin.size());
            std::vector<CTxOut> spent_outputs;
            spent_outputs.reserve(spent_outputs_len);
            for (size_t i = 0; i < spent_outputs_len; i++) {
                const CTxOut& tx_out{aixk_TransactionOutput::get(spent_outputs_[i])};
                spent_outputs.push_back(tx_out);
            }
            aixk_PrecomputedTransactionData::get(txdata).Init(tx, std::move(spent_outputs));
        } else {
            aixk_PrecomputedTransactionData::get(txdata).Init(tx, {});
        }

        return txdata;
    } catch (...) {
        return nullptr;
    }
}

aixk_PrecomputedTransactionData* aixk_precomputed_transaction_data_copy(const aixk_PrecomputedTransactionData* precomputed_txdata)
{
    return aixk_PrecomputedTransactionData::copy(precomputed_txdata);
}

void aixk_precomputed_transaction_data_destroy(aixk_PrecomputedTransactionData* precomputed_txdata)
{
    delete precomputed_txdata;
}

int aixk_script_pubkey_verify(const aixk_ScriptPubkey* script_pubkey,
                              const int64_t amount,
                              const aixk_Transaction* tx_to,
                              const aixk_PrecomputedTransactionData* precomputed_txdata,
                              const unsigned int input_index,
                              const aixk_ScriptVerificationFlags flags,
                              aixk_ScriptVerifyStatus* status)
{
    // Assert that all specified flags are part of the interface before continuing
    assert((flags & ~aixk_ScriptVerificationFlags_ALL) == 0);

    if (!is_valid_flag_combination(script_verify_flags::from_int(flags))) {
        if (status) *status = aixk_ScriptVerifyStatus_ERROR_INVALID_FLAGS_COMBINATION;
        return 0;
    }

    const CTransaction& tx{*aixk_Transaction::get(tx_to)};
    assert(input_index < tx.vin.size());

    const PrecomputedTransactionData& txdata{precomputed_txdata ? aixk_PrecomputedTransactionData::get(precomputed_txdata) : PrecomputedTransactionData(tx)};

    if (flags & aixk_ScriptVerificationFlags_TAPROOT && txdata.m_spent_outputs.empty()) {
        if (status) *status = aixk_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED;
        return 0;
    }

    if (status) *status = aixk_ScriptVerifyStatus_OK;

    bool result = VerifyScript(tx.vin[input_index].scriptSig,
                               aixk_ScriptPubkey::get(script_pubkey),
                               &tx.vin[input_index].scriptWitness,
                               script_verify_flags::from_int(flags),
                               TransactionSignatureChecker(&tx, input_index, amount, txdata, MissingDataBehavior::FAIL),
                               nullptr);
    return result ? 1 : 0;
}

aixk_TransactionInput* aixk_transaction_input_copy(const aixk_TransactionInput* input)
{
    return aixk_TransactionInput::copy(input);
}

const aixk_TransactionOutPoint* aixk_transaction_input_get_out_point(const aixk_TransactionInput* input)
{
    return aixk_TransactionOutPoint::ref(&aixk_TransactionInput::get(input).prevout);
}

void aixk_transaction_input_destroy(aixk_TransactionInput* input)
{
    delete input;
}

aixk_TransactionOutPoint* aixk_transaction_out_point_copy(const aixk_TransactionOutPoint* out_point)
{
    return aixk_TransactionOutPoint::copy(out_point);
}

uint32_t aixk_transaction_out_point_get_index(const aixk_TransactionOutPoint* out_point)
{
    return aixk_TransactionOutPoint::get(out_point).n;
}

const aixk_Txid* aixk_transaction_out_point_get_txid(const aixk_TransactionOutPoint* out_point)
{
    return aixk_Txid::ref(&aixk_TransactionOutPoint::get(out_point).hash);
}

void aixk_transaction_out_point_destroy(aixk_TransactionOutPoint* out_point)
{
    delete out_point;
}

aixk_Txid* aixk_txid_copy(const aixk_Txid* txid)
{
    return aixk_Txid::copy(txid);
}

void aixk_txid_to_bytes(const aixk_Txid* txid, unsigned char output[32])
{
    std::memcpy(output, aixk_Txid::get(txid).begin(), 32);
}

int aixk_txid_equals(const aixk_Txid* txid1, const aixk_Txid* txid2)
{
    return aixk_Txid::get(txid1) == aixk_Txid::get(txid2);
}

void aixk_txid_destroy(aixk_Txid* txid)
{
    delete txid;
}

void aixk_logging_set_options(const aixk_LoggingOptions options)
{
    LOCK(cs_main);
    LogInstance().m_log_timestamps = options.log_timestamps;
    LogInstance().m_log_time_micros = options.log_time_micros;
    LogInstance().m_log_threadnames = options.log_threadnames;
    LogInstance().m_log_sourcelocations = options.log_sourcelocations;
    LogInstance().m_always_print_category_level = options.always_print_category_levels;
}

void aixk_logging_set_level_category(aixk_LogCategory category, aixk_LogLevel level)
{
    LOCK(cs_main);
    if (category == aixk_LogCategory_ALL) {
        LogInstance().SetLogLevel(get_bclog_level(level));
    }

    LogInstance().AddCategoryLogLevel(get_bclog_flag(category), get_bclog_level(level));
}

void aixk_logging_enable_category(aixk_LogCategory category)
{
    LogInstance().EnableCategory(get_bclog_flag(category));
}

void aixk_logging_disable_category(aixk_LogCategory category)
{
    LogInstance().DisableCategory(get_bclog_flag(category));
}

void aixk_logging_disable()
{
    LogInstance().DisableLogging();
}

aixk_LoggingConnection* aixk_logging_connection_create(aixk_LogCallback callback, void* user_data, aixk_DestroyCallback user_data_destroy_callback)
{
    try {
        return aixk_LoggingConnection::create(callback, user_data, user_data_destroy_callback);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void aixk_logging_connection_destroy(aixk_LoggingConnection* connection)
{
    delete connection;
}

aixk_ChainParameters* aixk_chain_parameters_create(const aixk_ChainType chain_type)
{
    switch (chain_type) {
    case aixk_ChainType_MAINNET: {
        return aixk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::Main().release()));
    }
    case aixk_ChainType_TESTNET: {
        return aixk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet().release()));
    }
    case aixk_ChainType_TESTNET_4: {
        return aixk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet4().release()));
    }
    case aixk_ChainType_SIGNET: {
        return aixk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::SigNet({}).release()));
    }
    case aixk_ChainType_REGTEST: {
        return aixk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::RegTest({}).release()));
    }
    }
    assert(false);
}

aixk_ChainParameters* aixk_chain_parameters_copy(const aixk_ChainParameters* chain_parameters)
{
    return aixk_ChainParameters::copy(chain_parameters);
}

void aixk_chain_parameters_destroy(aixk_ChainParameters* chain_parameters)
{
    delete chain_parameters;
}

aixk_ContextOptions* aixk_context_options_create()
{
    return aixk_ContextOptions::create();
}

void aixk_context_options_set_chainparams(aixk_ContextOptions* options, const aixk_ChainParameters* chain_parameters)
{
    // Copy the chainparams, so the caller can free it again
    LOCK(aixk_ContextOptions::get(options).m_mutex);
    aixk_ContextOptions::get(options).m_chainparams = std::make_unique<const CChainParams>(aixk_ChainParameters::get(chain_parameters));
}

void aixk_context_options_set_notifications(aixk_ContextOptions* options, aixk_NotificationInterfaceCallbacks notifications)
{
    // The KernelNotifications are copy-initialized, so the caller can free them again.
    LOCK(aixk_ContextOptions::get(options).m_mutex);
    aixk_ContextOptions::get(options).m_notifications = std::make_shared<KernelNotifications>(notifications);
}

void aixk_context_options_set_validation_interface(aixk_ContextOptions* options, aixk_ValidationInterfaceCallbacks vi_cbs)
{
    LOCK(aixk_ContextOptions::get(options).m_mutex);
    aixk_ContextOptions::get(options).m_validation_interface = std::make_shared<KernelValidationInterface>(vi_cbs);
}

void aixk_context_options_destroy(aixk_ContextOptions* options)
{
    delete options;
}

aixk_Context* aixk_context_create(const aixk_ContextOptions* options)
{
    bool sane{true};
    const ContextOptions* opts = options ? &aixk_ContextOptions::get(options) : nullptr;
    auto context{std::make_shared<const Context>(opts, sane)};
    if (!sane) {
        LogError("Kernel context sanity check failed.");
        return nullptr;
    }
    return aixk_Context::create(context);
}

aixk_Context* aixk_context_copy(const aixk_Context* context)
{
    return aixk_Context::copy(context);
}

int aixk_context_interrupt(aixk_Context* context)
{
    return (*aixk_Context::get(context)->m_interrupt)() ? 0 : -1;
}

void aixk_context_destroy(aixk_Context* context)
{
    delete context;
}

const aixk_BlockTreeEntry* aixk_block_tree_entry_get_previous(const aixk_BlockTreeEntry* entry)
{
    if (!aixk_BlockTreeEntry::get(entry).pprev) {
        LogInfo("Genesis block has no previous.");
        return nullptr;
    }

    return aixk_BlockTreeEntry::ref(aixk_BlockTreeEntry::get(entry).pprev);
}

aixk_BlockValidationState* aixk_block_validation_state_create()
{
    return aixk_BlockValidationState::create();
}

aixk_BlockValidationState* aixk_block_validation_state_copy(const aixk_BlockValidationState* state)
{
    return aixk_BlockValidationState::copy(state);
}

void aixk_block_validation_state_destroy(aixk_BlockValidationState* state)
{
    delete state;
}

aixk_ValidationMode aixk_block_validation_state_get_validation_mode(const aixk_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = aixk_BlockValidationState::get(block_validation_state_);
    if (block_validation_state.IsValid()) return aixk_ValidationMode_VALID;
    if (block_validation_state.IsInvalid()) return aixk_ValidationMode_INVALID;
    return aixk_ValidationMode_INTERNAL_ERROR;
}

aixk_BlockValidationResult aixk_block_validation_state_get_block_validation_result(const aixk_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = aixk_BlockValidationState::get(block_validation_state_);
    switch (block_validation_state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        return aixk_BlockValidationResult_UNSET;
    case BlockValidationResult::BLOCK_CONSENSUS:
        return aixk_BlockValidationResult_CONSENSUS;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        return aixk_BlockValidationResult_CACHED_INVALID;
    case BlockValidationResult::BLOCK_INVALID_HEADER:
        return aixk_BlockValidationResult_INVALID_HEADER;
    case BlockValidationResult::BLOCK_MUTATED:
        return aixk_BlockValidationResult_MUTATED;
    case BlockValidationResult::BLOCK_MISSING_PREV:
        return aixk_BlockValidationResult_MISSING_PREV;
    case BlockValidationResult::BLOCK_INVALID_PREV:
        return aixk_BlockValidationResult_INVALID_PREV;
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        return aixk_BlockValidationResult_TIME_FUTURE;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        return aixk_BlockValidationResult_HEADER_LOW_WORK;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

aixk_ChainstateManagerOptions* aixk_chainstate_manager_options_create(const aixk_Context* context, const char* data_dir, size_t data_dir_len, const char* blocks_dir, size_t blocks_dir_len)
{
    if (data_dir == nullptr || data_dir_len == 0 || blocks_dir == nullptr || blocks_dir_len == 0) {
        LogError("Failed to create chainstate manager options: dir must be non-null and non-empty");
        return nullptr;
    }
    try {
        fs::path abs_data_dir{fs::absolute(fs::PathFromString({data_dir, data_dir_len}))};
        fs::create_directories(abs_data_dir);
        fs::path abs_blocks_dir{fs::absolute(fs::PathFromString({blocks_dir, blocks_dir_len}))};
        fs::create_directories(abs_blocks_dir);
        return aixk_ChainstateManagerOptions::create(aixk_Context::get(context), abs_data_dir, abs_blocks_dir);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager options: %s", e.what());
        return nullptr;
    }
}

void aixk_chainstate_manager_options_set_worker_threads_num(aixk_ChainstateManagerOptions* opts, int worker_threads)
{
    LOCK(aixk_ChainstateManagerOptions::get(opts).m_mutex);
    aixk_ChainstateManagerOptions::get(opts).m_chainman_options.worker_threads_num = worker_threads;
}

void aixk_chainstate_manager_options_destroy(aixk_ChainstateManagerOptions* options)
{
    delete options;
}

int aixk_chainstate_manager_options_set_wipe_dbs(aixk_ChainstateManagerOptions* chainman_opts, int wipe_block_tree_db, int wipe_chainstate_db)
{
    if (wipe_block_tree_db == 1 && wipe_chainstate_db != 1) {
        LogError("Wiping the block tree db without also wiping the chainstate db is currently unsupported.");
        return -1;
    }
    auto& opts{aixk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.wipe_data = wipe_block_tree_db == 1;
    opts.m_chainstate_load_options.wipe_chainstate_db = wipe_chainstate_db == 1;
    return 0;
}

void aixk_chainstate_manager_options_update_block_tree_db_in_memory(
    aixk_ChainstateManagerOptions* chainman_opts,
    int block_tree_db_in_memory)
{
    auto& opts{aixk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.memory_only = block_tree_db_in_memory == 1;
}

void aixk_chainstate_manager_options_update_chainstate_db_in_memory(
    aixk_ChainstateManagerOptions* chainman_opts,
    int chainstate_db_in_memory)
{
    auto& opts{aixk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_chainstate_load_options.coins_db_in_memory = chainstate_db_in_memory == 1;
}

aixk_ChainstateManager* aixk_chainstate_manager_create(
    const aixk_ChainstateManagerOptions* chainman_opts)
{
    auto& opts{aixk_ChainstateManagerOptions::get(chainman_opts)};
    std::unique_ptr<ChainstateManager> chainman;
    try {
        LOCK(opts.m_mutex);
        chainman = std::make_unique<ChainstateManager>(*opts.m_context->m_interrupt, opts.m_chainman_options, opts.m_blockman_options);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager: %s", e.what());
        return nullptr;
    }

    try {
        const auto chainstate_load_opts{WITH_LOCK(opts.m_mutex, return opts.m_chainstate_load_options)};

        kernel::CacheSizes cache_sizes{DEFAULT_KERNEL_CACHE};
        auto [status, chainstate_err]{node::LoadChainstate(*chainman, cache_sizes, chainstate_load_opts)};
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to load chain state from your data directory: %s", chainstate_err.original);
            return nullptr;
        }
        std::tie(status, chainstate_err) = node::VerifyLoadedChainstate(*chainman, chainstate_load_opts);
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to verify loaded chain state from your datadir: %s", chainstate_err.original);
            return nullptr;
        }
        if (auto result = chainman->ActivateBestChains(); !result) {
            LogError("%s", util::ErrorString(result).original);
            return nullptr;
        }
    } catch (const std::exception& e) {
        LogError("Failed to load chainstate: %s", e.what());
        return nullptr;
    }

    return aixk_ChainstateManager::create(std::move(chainman), opts.m_context);
}

const aixk_BlockTreeEntry* aixk_chainstate_manager_get_block_tree_entry_by_hash(const aixk_ChainstateManager* chainman, const aixk_BlockHash* block_hash)
{
    auto block_index = WITH_LOCK(aixk_ChainstateManager::get(chainman).m_chainman->GetMutex(),
                                 return aixk_ChainstateManager::get(chainman).m_chainman->m_blockman.LookupBlockIndex(aixk_BlockHash::get(block_hash)));
    if (!block_index) {
        LogDebug(BCLog::KERNEL, "A block with the given hash is not indexed.");
        return nullptr;
    }
    return aixk_BlockTreeEntry::ref(block_index);
}

const aixk_BlockTreeEntry* aixk_chainstate_manager_get_best_entry(const aixk_ChainstateManager* chainstate_manager)
{
    auto& chainman = *aixk_ChainstateManager::get(chainstate_manager).m_chainman;
    return aixk_BlockTreeEntry::ref(WITH_LOCK(chainman.GetMutex(), return chainman.m_best_header));
}

void aixk_chainstate_manager_destroy(aixk_ChainstateManager* chainman)
{
    {
        LOCK(aixk_ChainstateManager::get(chainman).m_chainman->GetMutex());
        for (const auto& chainstate : aixk_ChainstateManager::get(chainman).m_chainman->m_chainstates) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }

    delete chainman;
}

int aixk_chainstate_manager_import_blocks(aixk_ChainstateManager* chainman, const char** block_file_paths_data, size_t* block_file_paths_lens, size_t block_file_paths_data_len)
{
    try {
        std::vector<fs::path> import_files;
        import_files.reserve(block_file_paths_data_len);
        for (uint32_t i = 0; i < block_file_paths_data_len; i++) {
            if (block_file_paths_data[i] != nullptr) {
                import_files.emplace_back(std::string{block_file_paths_data[i], block_file_paths_lens[i]}.c_str());
            }
        }
        auto& chainman_ref{*aixk_ChainstateManager::get(chainman).m_chainman};
        node::ImportBlocks(chainman_ref, import_files);
        WITH_LOCK(::cs_main, chainman_ref.UpdateIBDStatus());
    } catch (const std::exception& e) {
        LogError("Failed to import blocks: %s", e.what());
        return -1;
    }
    return 0;
}

aixk_Block* aixk_block_create(const void* raw_block, size_t raw_block_length)
{
    if (raw_block == nullptr && raw_block_length != 0) {
        return nullptr;
    }
    auto block{std::make_shared<CBlock>()};

    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block), raw_block_length}};

    try {
        stream >> TX_WITH_WITNESS(*block);
    } catch (...) {
        LogDebug(BCLog::KERNEL, "Block decode failed.");
        return nullptr;
    }

    return aixk_Block::create(block);
}

aixk_Block* aixk_block_copy(const aixk_Block* block)
{
    return aixk_Block::copy(block);
}

size_t aixk_block_count_transactions(const aixk_Block* block)
{
    return aixk_Block::get(block)->vtx.size();
}

const aixk_Transaction* aixk_block_get_transaction_at(const aixk_Block* block, size_t index)
{
    assert(index < aixk_Block::get(block)->vtx.size());
    return aixk_Transaction::ref(&aixk_Block::get(block)->vtx[index]);
}

aixk_BlockHeader* aixk_block_get_header(const aixk_Block* block)
{
    const auto& block_ptr = aixk_Block::get(block);
    return aixk_BlockHeader::create(static_cast<const CBlockHeader&>(*block_ptr));
}

int aixk_block_to_bytes(const aixk_Block* block, aixk_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(*aixk_Block::get(block));
        return 0;
    } catch (...) {
        return -1;
    }
}

aixk_BlockHash* aixk_block_get_hash(const aixk_Block* block)
{
    return aixk_BlockHash::create(aixk_Block::get(block)->GetHash());
}

void aixk_block_destroy(aixk_Block* block)
{
    delete block;
}

aixk_Block* aixk_block_read(const aixk_ChainstateManager* chainman, const aixk_BlockTreeEntry* entry)
{
    auto block{std::make_shared<CBlock>()};
    if (!aixk_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlock(*block, aixk_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block.");
        return nullptr;
    }
    return aixk_Block::create(block);
}

aixk_BlockHeader* aixk_block_tree_entry_get_block_header(const aixk_BlockTreeEntry* entry)
{
    return aixk_BlockHeader::create(aixk_BlockTreeEntry::get(entry).GetBlockHeader());
}

int32_t aixk_block_tree_entry_get_height(const aixk_BlockTreeEntry* entry)
{
    return aixk_BlockTreeEntry::get(entry).nHeight;
}

const aixk_BlockHash* aixk_block_tree_entry_get_block_hash(const aixk_BlockTreeEntry* entry)
{
    return aixk_BlockHash::ref(aixk_BlockTreeEntry::get(entry).phashBlock);
}

int aixk_block_tree_entry_equals(const aixk_BlockTreeEntry* entry1, const aixk_BlockTreeEntry* entry2)
{
    return &aixk_BlockTreeEntry::get(entry1) == &aixk_BlockTreeEntry::get(entry2);
}

aixk_BlockHash* aixk_block_hash_create(const unsigned char block_hash[32])
{
    return aixk_BlockHash::create(std::span<const unsigned char>{block_hash, 32});
}

aixk_BlockHash* aixk_block_hash_copy(const aixk_BlockHash* block_hash)
{
    return aixk_BlockHash::copy(block_hash);
}

void aixk_block_hash_to_bytes(const aixk_BlockHash* block_hash, unsigned char output[32])
{
    std::memcpy(output, aixk_BlockHash::get(block_hash).begin(), 32);
}

int aixk_block_hash_equals(const aixk_BlockHash* hash1, const aixk_BlockHash* hash2)
{
    return aixk_BlockHash::get(hash1) == aixk_BlockHash::get(hash2);
}

void aixk_block_hash_destroy(aixk_BlockHash* hash)
{
    delete hash;
}

aixk_BlockSpentOutputs* aixk_block_spent_outputs_read(const aixk_ChainstateManager* chainman, const aixk_BlockTreeEntry* entry)
{
    auto block_undo{std::make_shared<CBlockUndo>()};
    if (aixk_BlockTreeEntry::get(entry).nHeight < 1) {
        LogDebug(BCLog::KERNEL, "The genesis block does not have any spent outputs.");
        return aixk_BlockSpentOutputs::create(block_undo);
    }
    if (!aixk_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlockUndo(*block_undo, aixk_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block spent outputs data.");
        return nullptr;
    }
    return aixk_BlockSpentOutputs::create(block_undo);
}

aixk_BlockSpentOutputs* aixk_block_spent_outputs_copy(const aixk_BlockSpentOutputs* block_spent_outputs)
{
    return aixk_BlockSpentOutputs::copy(block_spent_outputs);
}

size_t aixk_block_spent_outputs_count(const aixk_BlockSpentOutputs* block_spent_outputs)
{
    return aixk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size();
}

const aixk_TransactionSpentOutputs* aixk_block_spent_outputs_get_transaction_spent_outputs_at(const aixk_BlockSpentOutputs* block_spent_outputs, size_t transaction_index)
{
    assert(transaction_index < aixk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size());
    const auto* tx_undo{&aixk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.at(transaction_index)};
    return aixk_TransactionSpentOutputs::ref(tx_undo);
}

void aixk_block_spent_outputs_destroy(aixk_BlockSpentOutputs* block_spent_outputs)
{
    delete block_spent_outputs;
}

aixk_TransactionSpentOutputs* aixk_transaction_spent_outputs_copy(const aixk_TransactionSpentOutputs* transaction_spent_outputs)
{
    return aixk_TransactionSpentOutputs::copy(transaction_spent_outputs);
}

size_t aixk_transaction_spent_outputs_count(const aixk_TransactionSpentOutputs* transaction_spent_outputs)
{
    return aixk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size();
}

void aixk_transaction_spent_outputs_destroy(aixk_TransactionSpentOutputs* transaction_spent_outputs)
{
    delete transaction_spent_outputs;
}

const aixk_Coin* aixk_transaction_spent_outputs_get_coin_at(const aixk_TransactionSpentOutputs* transaction_spent_outputs, size_t coin_index)
{
    assert(coin_index < aixk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size());
    const Coin* coin{&aixk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.at(coin_index)};
    return aixk_Coin::ref(coin);
}

aixk_Coin* aixk_coin_copy(const aixk_Coin* coin)
{
    return aixk_Coin::copy(coin);
}

uint32_t aixk_coin_confirmation_height(const aixk_Coin* coin)
{
    return aixk_Coin::get(coin).nHeight;
}

int aixk_coin_is_coinbase(const aixk_Coin* coin)
{
    return aixk_Coin::get(coin).IsCoinBase() ? 1 : 0;
}

const aixk_TransactionOutput* aixk_coin_get_output(const aixk_Coin* coin)
{
    return aixk_TransactionOutput::ref(&aixk_Coin::get(coin).out);
}

void aixk_coin_destroy(aixk_Coin* coin)
{
    delete coin;
}

int aixk_chainstate_manager_process_block(
    aixk_ChainstateManager* chainman,
    const aixk_Block* block,
    int* _new_block)
{
    bool new_block;
    auto result = aixk_ChainstateManager::get(chainman).m_chainman->ProcessNewBlock(aixk_Block::get(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    if (_new_block) {
        *_new_block = new_block ? 1 : 0;
    }
    return result ? 0 : -1;
}

int aixk_chainstate_manager_process_block_header(
    aixk_ChainstateManager* chainstate_manager,
    const aixk_BlockHeader* header,
    aixk_BlockValidationState* state)
{
    try {
        auto& chainman = aixk_ChainstateManager::get(chainstate_manager).m_chainman;
        auto result = chainman->ProcessNewBlockHeaders({&aixk_BlockHeader::get(header), 1}, /*min_pow_checked=*/true, aixk_BlockValidationState::get(state), /*ppindex=*/nullptr);

        return result ? 0 : -1;
    } catch (const std::exception& e) {
        LogError("Failed to process block header: %s", e.what());
        return -1;
    }
}

const aixk_Chain* aixk_chainstate_manager_get_active_chain(const aixk_ChainstateManager* chainman)
{
    return aixk_Chain::ref(&WITH_LOCK(aixk_ChainstateManager::get(chainman).m_chainman->GetMutex(), return aixk_ChainstateManager::get(chainman).m_chainman->ActiveChain()));
}

int aixk_chain_get_height(const aixk_Chain* chain)
{
    LOCK(::cs_main);
    return aixk_Chain::get(chain).Height();
}

const aixk_BlockTreeEntry* aixk_chain_get_by_height(const aixk_Chain* chain, int height)
{
    LOCK(::cs_main);
    return aixk_BlockTreeEntry::ref(aixk_Chain::get(chain)[height]);
}

int aixk_chain_contains(const aixk_Chain* chain, const aixk_BlockTreeEntry* entry)
{
    LOCK(::cs_main);
    return aixk_Chain::get(chain).Contains(&aixk_BlockTreeEntry::get(entry)) ? 1 : 0;
}

aixk_BlockHeader* aixk_block_header_create(const void* raw_block_header, size_t raw_block_header_len)
{
    if (raw_block_header == nullptr && raw_block_header_len != 0) {
        return nullptr;
    }
    auto header{std::make_unique<CBlockHeader>()};
    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block_header), raw_block_header_len}};

    try {
        stream >> *header;
    } catch (...) {
        LogError("Block header decode failed.");
        return nullptr;
    }

    return aixk_BlockHeader::ref(header.release());
}

aixk_BlockHeader* aixk_block_header_copy(const aixk_BlockHeader* header)
{
    return aixk_BlockHeader::copy(header);
}

aixk_BlockHash* aixk_block_header_get_hash(const aixk_BlockHeader* header)
{
    return aixk_BlockHash::create(aixk_BlockHeader::get(header).GetHash());
}

const aixk_BlockHash* aixk_block_header_get_prev_hash(const aixk_BlockHeader* header)
{
    return aixk_BlockHash::ref(&aixk_BlockHeader::get(header).hashPrevBlock);
}

uint32_t aixk_block_header_get_timestamp(const aixk_BlockHeader* header)
{
    return aixk_BlockHeader::get(header).nTime;
}

uint32_t aixk_block_header_get_bits(const aixk_BlockHeader* header)
{
    return aixk_BlockHeader::get(header).nBits;
}

int32_t aixk_block_header_get_version(const aixk_BlockHeader* header)
{
    return aixk_BlockHeader::get(header).nVersion;
}

uint32_t aixk_block_header_get_nonce(const aixk_BlockHeader* header)
{
    return aixk_BlockHeader::get(header).nNonce;
}

void aixk_block_header_destroy(aixk_BlockHeader* header)
{
    delete header;
}
