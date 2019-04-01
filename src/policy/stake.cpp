// Copyright (c) 2018 The XPChain Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <policy/stake.h>

#include <crypto/sha256.h>
#include <hash.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <stdio.h>
#include <key_io.h>
#include <validation.h>
#include <util.h>

bool IsCoinStakeTx(CTransactionRef tx, const Consensus::Params &consensusParams, uint256 &hashBlock,
                         CTransactionRef& prevTx) {
    if (tx->vin.size() != 1) {
        return error("%s: coinstake has too many inputs", __func__);
    }
    if (tx->vout.size() != 1) {
        return error("%s: coinstake has too many outputs", __func__);
    }

    if (!GetTransaction(tx->vin[0].prevout.hash, prevTx, consensusParams, hashBlock, true)) {
        return error("%s: unknown coinstake input", __func__);
    }

    if (prevTx->GetHash() != tx->vin[0].prevout.hash) {
        return error("%s: invalid coinstake input hash", __func__);
    }

    if (!IsDestinationSame(prevTx->vout[tx->vin[0].prevout.n].scriptPubKey, tx->vout[0].scriptPubKey)) {
        return error("%s: invalid coinstake output", __func__);
    }

    return true;
}



bool IsDestinationSame(const CScript& a, const CScript& b)
{
    txnouttype aType, bType;
    std::vector <std::vector<unsigned char>> aSol, bSol;

    if (!Solver(a, aType, aSol) || !Solver(b, bType, bSol)) {
        return false;
    }

    if (aSol.size() != 1 || bSol.size() != 1) {
        return false;
    }

    CTxDestination aDest,bDest;
    if(!ExtractDestination(a, aDest) || !ExtractDestination(b, bDest)) {
        return false;
    }

    if (aDest != bDest) {
        return false;
    }

    return true;
}