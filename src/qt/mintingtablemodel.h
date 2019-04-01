#ifndef BITCOIN_QT_MINTINGTABLEMODEL_H
#define BITCOIN_QT_MINTINGTABLEMODEL_H

#include <qt/bitcoinunits.h>

#include <QAbstractTableModel>
#include <QStringList>

#include <memory>

namespace interfaces {
class Handler;
}

class PlatformStyle;
class MintingTablePriv;
class MintingFilterProxy;
class KernelRecord;
class WalletModel;

class MintingTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit MintingTableModel(const PlatformStyle *platformStyle, WalletModel *parent = 0);
    ~MintingTableModel();

    enum ColumnIndex {
        TxHash = 0,
        Address = 1,
        Balance = 2,
        Age = 3,
        CoinDay = 4,
        MintProbability = 5,
        MintReward = 6
    };

    void setMintingProxyModel(MintingFilterProxy *mintingProxy);
    int rowCount(const QModelIndex &parent) const;
    int columnCount(const QModelIndex &parent) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    QModelIndex index(int row, int column, const QModelIndex & parent = QModelIndex()) const;

    void setMintingInterval(int interval);

private:
    WalletModel *walletModel;
    std::unique_ptr<interfaces::Handler> m_handler_transaction_changed;
    QStringList columns;
    int mintingInterval;
    MintingTablePriv *priv;
    MintingFilterProxy *mintingProxyModel;
    const PlatformStyle *platformStyle;

    QString lookupAddress(const std::string &address, bool tooltip) const;

    double getDayToMint(KernelRecord *wtx) const;
    QString formatDayToMint(KernelRecord *wtx) const;
    QString formatTxAddress(const KernelRecord *wtx, bool tooltip) const;
    QString formatTxHash(const KernelRecord *wtx) const;
    QString formatTxAge(const KernelRecord *wtx) const;
    QString formatTxBalance(const KernelRecord *wtx) const;
    QString formatTxCoinDay(const KernelRecord *wtx) const;
    QString formatTxPoSReward(KernelRecord *wtx) const;

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

public Q_SLOTS:
    void updateTransaction(const QString &hash, int status, bool showTransaction);
    void updateAge();
    void updateDisplayUnit();

    friend class MintingTablePriv;
};

#endif // BITCOIN_QT_MINTINGTABLEMODEL_H
