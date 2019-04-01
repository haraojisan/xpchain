// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <miner.h>

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/tx_verify.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <hash.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <script/standard.h>
#include <timedata.h>
#include <util.h>
#include <utilmoneystr.h>
#include <validationinterface.h>

#include <algorithm>
#include <queue>
#include <utility>
#include <txdb.h>
#include <index/txindex.h>
#include <key_io.h>
#include <kernel.h>
#include <warnings.h>
#include <boost/thread.hpp>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#include <wallet/coincontrol.h>
#endif

// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate of a transaction combined with all
// its ancestors.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockWeight = 0;

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options() {
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams& params, const Options& options) : chainparams(params)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions()
{
    // Block resource limits
    // If -blockmaxweight is not given, limit to DEFAULT_BLOCK_MAX_WEIGHT
    BlockAssembler::Options options;
    options.nBlockMaxWeight = gArgs.GetArg("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    if (gArgs.IsArgSet("-blockmintxfee")) {
        CAmount n = 0;
        ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n);
        options.blockMinFeeRate = CFeeRate(n);
    } else {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams& params) : BlockAssembler(params, DefaultOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}
#ifdef ENABLE_WALLET
static std::vector<std::pair<CTxDestination, int>> GetRewardPct(const CWallet& wallet, const CTxDestination& defaultDestination)
{
    std::vector<std::pair<CTxDestination, int>> result;
    int remainder = 100;
    for(std::pair<std::string, uint8_t> p:wallet.vRewardDistributionPcts)
    {
        result.push_back(std::make_pair(DecodeDestination(p.first), (int)p.second));
        remainder -= (int)p.second;
        assert(remainder >= 0);
    }
    if(remainder > 0)
    {
        result.push_back(std::make_pair(defaultDestination, remainder));
    }
    return result;
}
static bool SignBlock(CBlock* pblock, const CWallet& wallet)
{
    assert(pblock->vtx.size() >= 2);
    CMutableTransaction coinbaseTx(*pblock->vtx[0]);
    std::vector<CPubKey> vPubKeys;
    LogPrintf("get pubkey\n");
    if (!GetPubKeysFromCoinStakeTx(pblock->vtx[1], vPubKeys)) {
        LogPrintf("could not get pubkey from TX\n");
        return false;
    }
    std::vector<unsigned char> sig;
    for (CPubKey pubkey : vPubKeys) {
        CKey privkey;
        if (wallet.GetKey(pubkey.GetID(), privkey)) {
            LogPrintf("sign block\n");
            if (privkey.Sign(pblock->GetBlockHeader().GetHash(), sig)) {
                //ScriptSig = Height Signature
                coinbaseTx.vin[0].scriptSig = coinbaseTx.vin[0].scriptSig << sig;
                pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
                pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
                LogPrintf("sign hash = %s signature = %s\n", pblock->GetBlockHeader().GetHash().ToString().c_str(), HexStr(sig.begin(), sig.end()));
                return true;
            }
        }
    }
    LogPrintf("could not get pubkey from wallet\n");
    return false;
}
#endif
std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, bool fMineWitnessTx)
{
#ifdef ENABLE_WALLET
    return CreateNewBlock(scriptPubKeyIn, nullptr, 0, 0, nullptr, 0, nullptr, fMineWitnessTx);
#else
    return CreateNewBlock(scriptPubKeyIn, 0, 0, nullptr, 0, nullptr, fMineWitnessTx);
#endif
}
#ifdef ENABLE_WALLET
std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CWallet* pwallet, uint32_t nTime, unsigned int nBits, CTransactionRef txCoinStake, CAmount nCoinStakeFee, CBlockIndex* pIndexLast, bool fMineWitnessTx)
#else
std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, uint32_t nTime, unsigned int nBits, CTransactionRef txCoinStake, CAmount nCoinStakeFee, CBlockIndex* pIndexLast, bool fMineWitnessTx)
#endif
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    LOCK2(cs_main, mempool.cs);
    if((pIndexLast) && (chainActive.Tip() != pIndexLast))
    {
        return nullptr;
    }

    CBlockIndex* pindexPrev = chainActive.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    bool fPoSHeight = IsPoSHeight(nHeight, chainparams.GetConsensus());

    if(fPoSHeight)
    {
        //add dummy "Send to myself" Tx as 2nd transaction
        pblock->vtx.emplace_back();
        pblocktemplate->vTxFees.push_back(-1); // updated at end
        pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end
    }

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);

    pblock->nTime = fPoSHeight?nTime:GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization).
    // Note that the mempool would accept transactions with witness data before
    // IsWitnessEnabled, but we would only ever mine blocks after IsWitnessEnabled
    // unless there is a massive block reorganization with the witness softfork
    // not activated.
    // TODO: replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockWeight = nBlockWeight;

    pblock->nBits          = fPoSHeight?nBits:GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());

    CScript scriptPubKey;

    if(fPoSHeight)
    {
        assert(txCoinStake);
        assert(txCoinStake->vout.size() == 1);
        pblock->vtx[1] = txCoinStake;
        nBlockTx++;
        scriptPubKey = pblock->vtx[1]->vout[0].scriptPubKey;
        //Correct?
        pblocktemplate->vTxSigOpsCost[1] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[1]);

        nFees += nCoinStakeFee;
        nBlockWeight += GetTransactionWeight(*txCoinStake);
    }
    else
    {
        scriptPubKey = scriptPubKeyIn;
    }

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();

    if(!fPoSHeight)
    {
        coinbaseTx.vout.resize(1);
        coinbaseTx.vout[0].scriptPubKey = scriptPubKey;
        coinbaseTx.vout[0].nValue = nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus());
    }
    else
    {
        bool splitcoinbase = true;
        txnouttype type;
        std::vector<std::vector<unsigned char>>ret;
        if(!Solver(scriptPubKey, type, ret))
        {
            return nullptr;
        }
#ifdef ENABLE_WALLET
        CWalletTx* pprevTx;
        {
            assert(txCoinStake->vin.size() == 1);
            LOCK(pwallet->cs_wallet);
            auto it = pwallet->mapWallet.find(txCoinStake->vin[0].prevout.hash);
            if(it == pwallet->mapWallet.end()){
                splitcoinbase = false;
            }
            pprevTx = &(it->second);
        }
        CBlockIndex* pprevBlockIndex = LookupBlockIndex(pprevTx->hashBlock);
        CAmount nBlockReward = GetProofOfStakeReward(nHeight, pprevTx->tx->vout[txCoinStake->vin[0].prevout.n].nValue, pblock->nTime - pprevBlockIndex->nTime, chainparams.GetConsensus());

        CTxDestination defaultDest;
        if(!ExtractDestination(scriptPubKey, defaultDest)) {
            return nullptr;
        }
        std::vector<std::pair<CTxDestination, int>>rewardPct = GetRewardPct(*pwallet, defaultDest);
        //TODO:Implement function calc reward
        std::vector<std::pair<CScript, CAmount>> rewardValue;
        rewardValue.clear();
        rewardValue.resize(rewardPct.size());
        for(size_t i = 0;i<rewardPct.size();i++)
        {
            rewardValue[i].first = GetScriptForDestination(rewardPct[i].first);
            rewardValue[i].second = nBlockReward * rewardPct[i].second / 100;
        }
        coinbaseTx.vout.resize(rewardPct.size()+1);
        for(size_t i = 0;i<rewardPct.size();i++)
        {
            coinbaseTx.vout[i+1].scriptPubKey = rewardValue[i].first;
            coinbaseTx.vout[i+1].nValue = rewardValue[i].second;
        }

        CScript txSig;
        if(!CreateTxSig(*pwallet, pblock->nTime, pblock->vtx[1], rewardValue, txSig))
        {
            splitcoinbase = false;
        }
        coinbaseTx.vout[0].nValue = 0;
        coinbaseTx.vout[0].scriptPubKey = txSig;
#else
        splitcoinbase = false;
#endif
        if(!splitcoinbase)
        {
            uint256 hashBlock;
            CTransactionRef prevTx;
            if(!GetTransaction(txCoinStake->vin[0].prevout.hash, prevTx, chainparams.GetConsensus(), hashBlock, true)) {
                return nullptr;
            }
            assert(prevTx);
            CBlockIndex* pprevBlockIndex = LookupBlockIndex(hashBlock);
            assert(txCoinStake->vin.size() == 1);
            assert(pprevBlockIndex);
            CAmount nBlockReward = GetProofOfStakeReward(nHeight, prevTx->vout[txCoinStake->vin[0].prevout.n].nValue, pblock->nTime - pprevBlockIndex->nTime, chainparams.GetConsensus());

            coinbaseTx.vout.resize(1);
            coinbaseTx.vout[0].scriptPubKey = scriptPubKey;
            coinbaseTx.vout[0].nValue = nBlockReward;
        }
    }
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight;
    if (!fPoSHeight) {
        coinbaseTx.vin[0].scriptSig = coinbaseTx.vin[0].scriptSig << OP_0;
    }
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
    pblocktemplate->vTxFees[0] = -nFees;

    LogPrintf("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if(!fPoSHeight)
    {
        UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    }
    pblock->nNonce         = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * GetLegacySigOpCount(*pblock->vtx[0]);
    if (fPoSHeight) {
#ifdef ENABLE_WALLET
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        if (!SignBlock(pblock, *pwallet)) {
            return nullptr;
        }
#else
        return nullptr;
#endif
    }

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    LogPrint(BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n", 0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1), 0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost) const
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package)
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
        indexed_modified_transaction_set &mapModifiedTx)
{
    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                    nBlockMaxWeight - 4000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        for (size_t i=0; i<sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock* pblock, const CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
#ifdef ENABLE_WALLET

static unsigned int GetnBits(const CBlockIndex* pIndexLast, Consensus::Params params)
{
    assert(pIndexLast);
    assert(pIndexLast->pprev);
    assert(pIndexLast->nHeight + 1 > params.nSwitchHeight);
    return CalculateNextWorkRequired(pIndexLast, pIndexLast->pprev->GetBlockTime(), params);
}

static bool GetPrevBlockIndex(const COutput& coin, CBlockIndex** pIndex)
{
    LOCK(cs_main);
    *pIndex = LookupBlockIndex(coin.tx->hashBlock);
    if (*pIndex == nullptr) {
        return false;
    }
    return true;
}

void BitcoinMinter(const std::shared_ptr<CWallet>& wallet)
{
    LogPrintf("CPUMiner started for proof-of-stake\n");
    RenameThread("xpchain-stake-minter");

    /*
    TODO:write warning if cannot stake
    */
    try
    {
        while (true)
        {
            int64_t start = GetTimeMillis();
            /*TODO:
            regtest always stake
            others always mint GetBoolArg or !vNodes.empty()
            */

            if(wallet->IsLocked())
            {
                MilliSleep(1000);
                continue;
            }

            if(IsInitialBlockDownload()) {
                MilliSleep(1000);
                continue;
            }

            //
            // Create new block
            //
            if(!IsPoSHeight(chainActive.Height()+1,Params().GetConsensus()))
            {
                MilliSleep(1000);
                continue;
            }

            CBlockIndex* pIndexLast = chainActive.Tip();
            assert(pIndexLast);

            unsigned int nBits = GetnBits(pIndexLast, Params().GetConsensus());
            uint32_t nTime = std::max(GetAdjustedTime(), pIndexLast->GetMedianTimePast()+1);

            std::vector<COutput> vCoins;
            {
                LOCK2(cs_main,wallet->cs_wallet);
                wallet->AvailableCoins(vCoins);
            }

            for (COutput coin : vCoins) {
                CBlockIndex* pprevIndex;
                if(!GetPrevBlockIndex(coin, &pprevIndex)){
                    continue;
                }
                uint256 hashProofOfStake;
                CBlock prevblock;
                assert(pprevIndex);
                if(!ReadBlockFromDisk(prevblock, pprevIndex, Params().GetConsensus())) {
                    continue;
                }
                if(CheckStakeKernelHash(nBits, pprevIndex->GetBlockTime(), GetSizeOfCompactSize(prevblock.vtx.size())+sizeof(CBlockHeader), coin.tx->tx->vout[coin.i].nValue, coin.i, nTime, hashProofOfStake))
                {
                    CScript scriptDummy;
                    CAmount nFees;
                    CTransactionRef txCoinStake;
                    txnouttype t;
                    std::vector<std::vector<unsigned char>> a;
                    Solver(coin.GetInputCoin().txout.scriptPubKey, t, a);
                    if(!wallet->CreateCoinStake(coin, txCoinStake, nFees))
                    {
                        continue;
                    }
                    std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(scriptDummy, wallet.get(), nTime, nBits, txCoinStake, nFees, pIndexLast));
                    if(!pblocktemplate.get())
                    {
                        continue;
                    }
                    else
                    {
                        CBlock *pblock = &pblocktemplate->block;
                        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
                        if (!ProcessNewBlock(Params(), shared_pblock, true, nullptr))
                        {
                            continue;
                        }
                        LogPrintf("success! hash = %s\n", pblock->GetHash().ToString().c_str());
                        break;
                    }
                }
            }
            int64_t end = GetTimeMillis();
            MilliSleep(std::max(0ll, 1000ll - (end - start)));
        }
    }
    catch (boost::thread_interrupted)
    {
        LogPrintf("XPChainMiner terminated\n");
        return;
    }
}

void static ThreadStakeMinter(const std::shared_ptr<CWallet>& wallet)
{
    LogPrintf("ThreadStakeMinter started %s\n", wallet->GetName());
    if (!gArgs.GetBoolArg("-minting", true))
    {
        LogPrintf("nominting\n");
        return;
    }

    while(true){
        try
        {
            BitcoinMinter(wallet);
            LogPrintf("ThreadStakeMinter exiting\n");
            return; // correct exiting
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ThreadStakeMinter()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ThreadStakeMinter()");
        }
        LogPrintf("ThreadStakeMinter restarting...\n");
        MilliSleep(1000);
    }
}

void MintStake(boost::thread_group& threadGroup, const std::shared_ptr<CWallet>& wallet)
{
    threadGroup.create_thread(boost::bind(&ThreadStakeMinter, wallet));
}

bool CreateTxSig(const CWallet& wallet, uint32_t nTime, CTransactionRef txCoinStake, const std::vector<std::pair<CScript, CAmount>>& vValues, CScript& script)
{
    txnouttype type;
    std::vector<std::vector<unsigned char>>ret;
    if(!Solver(txCoinStake->vout[0].scriptPubKey, type, ret))
    {
        LogPrintf("solver failed\n");
        return false;
    }
    if(type == WITNESS_V0_SCRIPTHASH_SIZE || type == TX_MULTISIG || type == TX_PUBKEY)
    {
        return false;
    }
    CTxDestination dest;
    if(!ExtractDestination(txCoinStake->vout[0].scriptPubKey, dest))
    {
        LogPrintf("address not found\n");
        return false;
    }

    //TODO: vValues size < 2^32-1

    //get pubkey hash from dest
    CKeyID keyID =  GetKeyForDestination(wallet, dest);
    if(keyID.IsNull())
    {
        LogPrintf("pubkey hash not found\n");
        return false;
    }

    //get privkey of pubkeyhash from wallet
    CKey key;
    if(!wallet.GetKey(keyID, key))
    {
        LogPrintf("privkey not found\n");
        return false;
    }

    //calc pubkey
    CPubKey pubkey = key.GetPubKey();
    std::vector<unsigned char> vchSig;
    uint256 hash = GetRewardHash(vValues, txCoinStake, nTime);
    bool result = key.Sign(hash, vchSig, 0);

    //script is  OP_RETURN CScriptNum.size() vValues.size() signature_size vchSig pubkey_size pubkey
    script = CScript() << OP_RETURN << CScriptNum((int64_t)vValues.size()) << vchSig << ToByteVector(pubkey);
    if(!result)
    {
        LogPrintf("sign failed\n");
    }
    return result;
}
#endif