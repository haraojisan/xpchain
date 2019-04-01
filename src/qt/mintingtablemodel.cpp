#include <qt/transactiontablemodel.h>

#include <qt/addresstablemodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactiondesc.h>
#include <qt/transactionrecord.h>
#include <qt/walletmodel.h>
#include <qt/mintingtablemodel.h>
#include <qt/mintingfilterproxy.h>

#include <core_io.h>
#include <interfaces/handler.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <validation.h>
#include <sync.h>
#include <uint256.h>
#include <util.h>
#include <key_io.h>
#include <consensus/consensus.h>
#include <timedata.h>
#include <stdint.h>
#include <chain.h>
#include <chainparams.h>
#include <math.h>

#include <rpc/blockchain.h>

#include <kernelrecord.h>
#include <kernel.h>

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QLocale>
#include <QList>
#include <QColor>
#include <QTimer>
#include <QIcon>
#include <QDebug>
#include <QDateTime>
#include <QtAlgorithms>

// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
    Qt::AlignLeft|Qt::AlignVCenter,
    Qt::AlignLeft|Qt::AlignVCenter,
    Qt::AlignRight|Qt::AlignVCenter,
    Qt::AlignRight|Qt::AlignVCenter,
    Qt::AlignRight|Qt::AlignVCenter,
    Qt::AlignRight|Qt::AlignVCenter,
    Qt::AlignRight|Qt::AlignVCenter
};

// Comparison operator for sort/binary search of model tx list
struct TxLessThan
{
    bool operator()(const KernelRecord &a, const KernelRecord &b) const
    {
        return a.hash < b.hash || (a.hash == b.hash && a.n < b.n);
    }
    bool operator()(const KernelRecord &a, const std::pair<uint256, uint32_t> &b) const
    {
        return a.hash < b.first || (a.hash == b.first && a.n < b.second);
    }
    bool operator()(const std::pair<uint256, uint32_t> &a, const KernelRecord &b) const
    {
        return a.first < b.hash || (a.first == b.hash && a.second < b.n);
    }
    bool operator()(const KernelRecord &a, const uint256 &b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256 &a, const KernelRecord &b) const
    {
        return a < b.hash;
    }
};

bool compareWtx(const interfaces::WalletTx &a, const interfaces::WalletTx &b)
{
  return a.time < b.time;
}
// Private implementation
class MintingTablePriv
{
public:
    MintingTablePriv(MintingTableModel *_parent):
            parent(_parent)
    {
    }

    MintingTableModel *parent;

    /* Local cache of wallet.
     * As it is in the same order as the CWallet, by definition
     * this is sorted by sha256.
     */
    QList<KernelRecord> cachedWallet;
    QList<std::pair<uint256, int>> procQueue;

    /* Query entire wallet anew from core.
     */
    void refreshWallet()
    {
        qDebug() << "MintingTablePriv::refreshWallet";

        int dequeue = std::min(procQueue.size(), 100); // limit 100
        while(dequeue-- > 0)
        {
            // dequeue one event
            std::pair<uint256, uint32_t> pair = procQueue[0];
            procEvent(pair.first, pair.second);
            procQueue.removeAt(0);
        }
    }

    void procEvent(uint256 hash, int status)
    {
        // Find bounds of this transaction in model
        QList<KernelRecord>::iterator lower = qLowerBound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        QList<KernelRecord>::iterator upper = qUpperBound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        int lowerIndex = (lower - cachedWallet.begin());
        int upperIndex = (upper - cachedWallet.begin());

        interfaces::WalletTx wtx = parent->walletModel->wallet().getWalletTx(hash);
        interfaces::WalletTxStatus tx_status;
        int num_blocks;
        int64_t block_time;
        bool success = parent->walletModel->wallet().tryGetTxStatus(hash, tx_status, num_blocks, block_time);

        if(!success){
            // not found status
            return;
        }

        if(status == CT_NEW)
        {
            // requeue if the transaction is coinbase and immature
            if(tx_status.is_coinbase && tx_status.blocks_to_maturity > 0)
            {
                procQueue.append(std::make_pair(hash, status));
                return;
            }

            // spent
            const std::vector<CTxIn> ins = wtx.tx->vin;
            const std::vector<isminetype> isMine = wtx.txin_is_mine;
            for(uint32_t i = 0; i < ins.size(); i++)
            {
                if(isMine[i] == isminetype::ISMINE_SPENDABLE)
                {
                    uint256 phash = ins[i].prevout.hash;
                    uint32_t n = ins[i].prevout.n;

                    for(int i = 0; i < cachedWallet.size(); i++){
                        if(cachedWallet[i].hash == phash && cachedWallet[i].n == n){
                            parent->beginRemoveRows(QModelIndex(), i, i);
                            cachedWallet.removeAt(i);
                            parent->endRemoveRows();
                            break;
                        }
                    }
                }
            }

            // append
            int offsetLower = 0;
            for(const KernelRecord& kr : KernelRecord::decomposeOutput(wtx))
            {
                if(parent->walletModel->wallet().isSpent(kr.hash, kr.n)) // spent
                {
                    continue;
                }

                parent->beginInsertRows(QModelIndex(), lowerIndex + offsetLower, lowerIndex + offsetLower);
                cachedWallet.insert(lowerIndex + offsetLower, kr);
                parent->endInsertRows();
                offsetLower++;
            }
        }
        else if(status == CT_UPDATED)
        {
            // nothing to do
        }
        else if(status == CT_DELETED)
        {
            // this status is not thrown
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex - 1);
            for(int i = lowerIndex; i < upperIndex; i++)
            {
                cachedWallet.removeAt(i);
            }
            parent->endRemoveRows();

            for(uint32_t n = 0; n <= wtx.tx->vin.size(); n++)
            {
                if(wtx.txin_is_mine[n] == isminetype::ISMINE_SPENDABLE)
                {
                    interfaces::WalletTx prev_wtx = parent->walletModel->wallet().getWalletTx(wtx.tx->vin[n].prevout.hash);

                    std::vector<KernelRecord> krs = KernelRecord::decomposeOutput(prev_wtx);
                    const KernelRecord& kr = krs[wtx.tx->vin[n].prevout.n];

                    QList<KernelRecord>::iterator prev_lower = qLowerBound(
                        cachedWallet.begin(), cachedWallet.end(), kr, TxLessThan());
                    QList<KernelRecord>::iterator prev_upper = qUpperBound(
                        cachedWallet.begin(), cachedWallet.end(), kr, TxLessThan());
                    int prev_lowerIndex = (prev_lower - cachedWallet.begin());

                    if(prev_lower == prev_upper) // not in model
                    {
                        parent->beginInsertRows(QModelIndex(), prev_lowerIndex, prev_lowerIndex);
                        cachedWallet.insert(prev_lowerIndex, kr);
                        parent->endInsertRows();
                    }
                }
            }
        }
        else{
            qDebug() << "MintingTablePriv::procEvent : Unexpected status";
        }
    }

    int size()
    {
        return cachedWallet.size();
    }

    KernelRecord *index(int idx)
    {
        if(idx >= 0 && idx < cachedWallet.size())
        {
            KernelRecord *rec = &cachedWallet[idx];
            return rec;
        }
        else
        {
            return 0;
        }
    }

    QString describe(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord *rec, int unit)
    {
        /*{
            LOCK(wallet->cs_wallet);
            std::map<uint256, CWalletTx>::iterator mi = wallet->mapWallet.find(rec->hash);
            if(mi != wallet->mapWallet.end())
            {*/
                return TransactionDesc::toHTML(node, wallet, rec, unit);
            /*}
        }
        return QString("");*/
    }

};

MintingTableModel::MintingTableModel(const PlatformStyle *_platformStyle, WalletModel *parent):
        QAbstractTableModel(parent),
        walletModel(parent),
        mintingInterval(10),
        priv(new MintingTablePriv(this)),
        //cachedNumBlocks(0),
        platformStyle(_platformStyle)
{
    columns << tr("Transaction") <<  tr("Address") << tr("Balance") << tr("Age") << tr("CoinDay") << tr("MintProbability") << tr("MintReward");

    // Initialize records
    std::vector<interfaces::WalletTx> wtxs = walletModel->wallet().getWalletTxs();
    std::sort(wtxs.begin(), wtxs.end(), compareWtx);
    for (const auto& wtx : wtxs)
    {
        priv->procEvent(wtx.tx->GetHash(), CT_NEW);
    }

    subscribeToCoreSignals();
    priv->refreshWallet();

    QTimer *timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateAge()));
    timer->start(MODEL_UPDATE_DELAY);

    connect(walletModel->getOptionsModel(), SIGNAL(displayUnitChanged(int)), this, SLOT(updateDisplayUnit()));
}

MintingTableModel::~MintingTableModel()
{
    unsubscribeFromCoreSignals();
    delete priv;
}

void MintingTableModel::updateTransaction(const QString &hash, int status, bool showTransaction)
{
    uint256 updated;
    updated.SetHex(hash.toStdString());

    // CT_NEW -> add
    // !showTransaction -> delete
    // CT_UPDATED -> nothing to do
    priv->procQueue.append(std::make_pair(updated, showTransaction ? status : CT_DELETED));
}

void MintingTableModel::updateAge()
{
    Q_EMIT dataChanged(index(0, Age), index(priv->size()-1, Age));
    Q_EMIT dataChanged(index(0, CoinDay), index(priv->size()-1, CoinDay));
    Q_EMIT dataChanged(index(0, MintProbability), index(priv->size()-1, MintProbability));
    Q_EMIT dataChanged(index(0, MintReward), index(priv->size()-1, MintReward));
    priv->refreshWallet();
}

void MintingTableModel::setMintingProxyModel(MintingFilterProxy *mintingProxy)
{
    mintingProxyModel = mintingProxy;
}

int MintingTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int MintingTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

QVariant MintingTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid())
        return QVariant();
    KernelRecord *rec = static_cast<KernelRecord*>(index.internalPointer());

    switch(role)
    {
      case Qt::DisplayRole:
        switch(index.column())
        {
        case Address:
            return formatTxAddress(rec, false);
        case TxHash:
            return formatTxHash(rec);
        case Age:
            return formatTxAge(rec);
        case Balance:
            return formatTxBalance(rec);
        case CoinDay:
            return formatTxCoinDay(rec);
        case MintProbability:
            return formatDayToMint(rec);
        case MintReward:
            return formatTxPoSReward(rec);
        }
        break;
      case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
        break;
      case Qt::ToolTipRole:
        switch(index.column())
        {
        case MintProbability:
            int interval = this->mintingInterval;
            QString unit = tr("minutes");

            int hours = interval / 60;
            int days = hours  / 24;

            if(hours > 1) {
                interval = hours;
                unit = tr("hours");
            }
            if(days > 1) {
                interval = days;
                unit = tr("days");
            }

            QString str = QString(tr("You have %1 chance to find a POS block if you mint %2 %3 at current difficulty."));
            return str.arg(index.data().toString().toUtf8().constData()).arg(interval).arg(unit);
        }
        break;
      case Qt::EditRole:
        switch(index.column())
        {
        case Address:
            return formatTxAddress(rec, false);
        case TxHash:
            return formatTxHash(rec);
        case Age:
            return (qlonglong)rec->getAge();
        case CoinDay:
            return (qlonglong)rec->getCoinDay();
        case Balance:
            return (qlonglong)rec->nValue;
        case MintProbability:
            return getDayToMint(rec);
        case MintReward:
            return formatTxPoSReward(rec);
        }
        break;
      case Qt::BackgroundColorRole:
        int minAge = Params().GetConsensus().nStakeMinAge / 60 / 60 / 24;
        int maxAge = Params().GetConsensus().nStakeMaxAge / 60 / 60 / 24;
        if(rec->getAge() < minAge)
        {
            return COLOR_MINT_YOUNG;
        }
        else if (rec->getAge() >= minAge && rec->getAge() < maxAge)
        {
            return COLOR_MINT_MATURE;
        }
        else
        {
            return COLOR_MINT_OLD;
        }
        break;

    }
    return QVariant();
}

void MintingTableModel::setMintingInterval(int interval)
{
    mintingInterval = interval;
}

QString MintingTableModel::lookupAddress(const std::string &address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if(!label.isEmpty())
    {
        description += label;
    }
    if(label.isEmpty() || tooltip)
    {
        description += QString(" (") + QString::fromStdString(address) + QString(")");
    }
    return description;
}

QString MintingTableModel::formatTxPoSReward(KernelRecord *wtx) const
{
    QString posReward;
    posReward += QString(QObject::tr("from  %1 to %2")).arg(BitcoinUnits::formatWithUnit(walletModel->getOptionsModel()->getDisplayUnit(), wtx->getPoSReward(0)),
        BitcoinUnits::formatWithUnit(walletModel->getOptionsModel()->getDisplayUnit(), wtx->getPoSReward(mintingInterval)));
    return posReward;
}
double MintingTableModel::getDayToMint(KernelRecord *wtx) const
{
    double difficulty = GetDifficulty(chainActive.Tip());

    double prob = wtx->getProbToMintWithinNMinutes(difficulty, mintingInterval);
    prob = prob * 100;
    return prob;
}

QString MintingTableModel::formatDayToMint(KernelRecord *wtx) const
{
    double prob = getDayToMint(wtx);
    return QString::number(prob, 'f', 6) + "%";
}

QString MintingTableModel::formatTxAddress(const KernelRecord *wtx, bool tooltip) const
{
    return QString::fromStdString(wtx->address);
}

QString MintingTableModel::formatTxHash(const KernelRecord *wtx) const
{
    return QString::fromStdString(wtx->hash.ToString());
}

QString MintingTableModel::formatTxCoinDay(const KernelRecord *wtx) const
{
    return QString::number(wtx->getCoinDay());
}

QString MintingTableModel::formatTxAge(const KernelRecord *wtx) const
{
    int64_t nAge = wtx->getAge();
    return QString::number(nAge);
}

QString MintingTableModel::formatTxBalance(const KernelRecord *wtx) const
{
    return BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->nValue);
}

QVariant MintingTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole)
        {
            return columns[section];
        }
        else if (role == Qt::TextAlignmentRole)
        {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole)
        {
            switch(section)
            {
            case Address:
                return tr("Destination address of the output.");
            case TxHash:
                return tr("Original transaction id.");
            case Age:
                return tr("Age of the transaction in days.");
            case Balance:
                return tr("Balance of the output.");
            case CoinDay:
                return tr("Coin age in the output.");
            case MintProbability:
                return tr("Chance to mint a block within given time interval.");
            case MintReward:
                return tr("The size of the potential rewards if the block is found at the beginning and the end given time interval.");
            }
        }
    }
    return QVariant();
}

QModelIndex MintingTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    KernelRecord *data = priv->index(row);
    if(data)
    {
        return createIndex(row, column, priv->index(row));
    }
    else
    {
        return QModelIndex();
    }
}

void MintingTableModel::updateDisplayUnit()
{
    // emit dataChanged to update Balance column with the current unit
    Q_EMIT dataChanged(index(0, Balance), index(priv->size()-1, Balance));
}

// queue notifications to show a non freezing progress dialog e.g. for rescan
struct TransactionNotification
{
public:
    TransactionNotification() {}
    TransactionNotification(uint256 _hash, ChangeType _status, bool _showTransaction):
        hash(_hash), status(_status), showTransaction(_showTransaction) {}

    void invoke(QObject *mtm)
    {
        QString strHash = QString::fromStdString(hash.GetHex());
        qDebug() << "NotifyTransactionChanged: " + strHash + " status= " + QString::number(status);
        QMetaObject::invokeMethod(mtm, "updateTransaction", Qt::QueuedConnection,
                                  Q_ARG(QString, strHash),
                                  Q_ARG(int, status),
                                  Q_ARG(bool, showTransaction));
    }
private:
    uint256 hash;
    ChangeType status;
    bool showTransaction;
};

static bool fQueueNotifications = false;
static std::vector< TransactionNotification > vQueueNotifications;

static void NotifyTransactionChanged(MintingTableModel *mtm, const uint256 &hash, ChangeType status)
{
    // Find transaction in wallet
    // Determine whether to show transaction or not (determine this here so that no relocking is needed in GUI thread)
    bool showTransaction = KernelRecord::showTransaction();

    TransactionNotification notification(hash, status, showTransaction);

    if (fQueueNotifications)
    {
        vQueueNotifications.push_back(notification);
        return;
    }
    notification.invoke(mtm);
}

void MintingTableModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_transaction_changed = walletModel->wallet().handleTransactionChanged(boost::bind(NotifyTransactionChanged, this, _1, _2));
}

void MintingTableModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_transaction_changed->disconnect();
}

