#include <chain.h>
#include <chainparams.h>
#include <index/txindex.h>
#include <init.h>
#include <kernel.h>
#include <script/interpreter.h>
#include <timedata.h>
#include <txdb.h>
#include <uint256.h>
#include <unordered_map>
#include <util.h>
#include <validation.h>

bool CheckStakeKernelHash(unsigned int nBits, uint32_t nTimeBlockFrom, unsigned int nTxPrevOffset, CAmount nAmount, uint64_t n, uint32_t nTimeTx, uint256& hashProofOfStake)
{
    if (nTimeBlockFrom + Params().GetConsensus().nStakeMinAge > nTimeTx) // Min age requirement
        return false;

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    int64_t nTimeWeight = std::min((int64_t)nTimeTx - nTimeBlockFrom, Params().GetConsensus().nStakeMaxAge) - Params().GetConsensus().nStakeMinAge;

    arith_uint256 bnCoinDayWeight = arith_uint256(nAmount) * nTimeWeight / COIN / (24 * 60 * 60);

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);

    ss << nBits << nTimeBlockFrom << nTxPrevOffset << nTimeBlockFrom << n << nTimeTx;

    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (arith_uint512(UintToArith256(hashProofOfStake)) > arith_uint512(bnCoinDayWeight) * arith_uint512(bnTargetPerCoinDay))
        return false;

    return true;
}

bool CheckStakeKernelHash(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTxOut& txOutPrev, const COutPoint& prevout, uint32_t nTimeTx, uint256& hashProofOfStake)
{
    return CheckStakeKernelHash(nBits, blockFrom.GetBlockTime(), nTxPrevOffset, txOutPrev.nValue, prevout.n, nTimeTx, hashProofOfStake);
}

bool CheckProofOfStake(const CTransactionRef& tx, unsigned int nBits, uint256& hashProofOfStake, unsigned int nBlockTime)
{
    const CTxIn& txin = tx->vin[0];

    CTransactionRef txTmp;
    uint256 hash;

    if (!GetTransaction(txin.prevout.hash, txTmp, Params().GetConsensus(), hash, true)) {
        return error("%s: txPrev not found hash = %s\n", __func__, txin.prevout.hash.ToString().c_str());
    }

    if (txTmp->GetHash() != txin.prevout.hash) {
        return false;
    }

    if (hash == uint256()) {
        return false;
    }

    // Verify signature
    PrecomputedTransactionData txdata(*tx);
    if (!CScriptCheck(txTmp->vout[tx->vin[0].prevout.n], *tx, 0, 0, true, &txdata)()) {
        return error("%s: VerifySignature failed on coinstake %s\n", __func__, tx->GetHash().ToString());
    }
    // Get transaction index for the previous transaction

    auto itr = mapBlockIndex.find(hash);
    if (itr == mapBlockIndex.end()) {
        return error("%s: blockindex not found hash = %s\n", __func__, hash.ToString().c_str());
    }

    CBlockIndex* pindex = (*itr).second;
    if (pindex->GetBlockHash() != hash) {
        return false;
    }

    CBlock block;
    if (!ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
        return error("%s: block not found db hash = %s\n", __func__, hash.ToString().c_str());
    }

    if (!CheckStakeKernelHash(nBits, block.GetBlockTime(), GetSizeOfCompactSize(block.vtx.size()) + sizeof(CBlockHeader), txTmp->vout[txin.prevout.n].nValue, txin.prevout.n, nBlockTime, hashProofOfStake))
        return false;

    return true;
}
