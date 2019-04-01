#ifndef BITCOIN_KERNELRECORD_H
#define BITCOIN_KERNELRECORD_H

#include <amount.h>
#include <uint256.h>

namespace interfaces {
class Node;
class Wallet;
struct WalletTx;
struct WalletTxStatus;
struct WalletTxOut;
}

class KernelRecord
{
public:
    KernelRecord():
        hash(), n(), nTime(0), address(""), nValue(0), prevMinutes(0), prevDifficulty(0), prevProbability(0)
    {
    }

    KernelRecord(uint256 hash, uint32_t n, int64_t nTime):
            hash(hash), n(n), nTime(nTime), address(""), nValue(0), prevMinutes(0), prevDifficulty(0), prevProbability(0)
    {
    }

    KernelRecord(uint256 hash, uint32_t n, int64_t nTime, const std::string &address, int64_t nValue):
        hash(hash), n(n), nTime(nTime), address(address), nValue(nValue), prevMinutes(0), prevDifficulty(0), prevProbability(0)
    {
    }

    static bool showTransaction();
    static std::vector<KernelRecord> decomposeOutput(const interfaces::WalletTx& wtx);

    uint256 hash;
    uint32_t n;
    int64_t nTime;
    std::string address;
    int64_t nValue;

    std::string getTxID();
    uint32_t getTxOutIndex();
    int64_t getAge() const;
    uint64_t getCoinDay() const;
    double getProbToMintStake(double difficulty, int timeOffset = 0) const;
    double getProbToMintWithinNMinutes(double difficulty, int minutes);
    int64_t getPoSReward(int minutes);
protected:
    int prevMinutes;
    double prevDifficulty;
    double prevProbability;
};

#endif // BITCOIN_KERNELRECORD_H
