XPChain Core version 0.17.0-3 is now available from:

  <https://www.xpchain.io/?loc=lnkwallet>

This is a new minor version release, including various bugfixes
and performance improvements.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/xpc-wg/xpchain/issues>

How to Upgrade
==============

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over `/Applications/XPChain-Qt` (on Mac).

Compatibility
==============

XPChain Core is extensively tested on multiple operating systems using
the Linux kernel, macOS 10.10+, and Windows 7 and newer (Windows XP is not supported).

XPChain Core should also work on most other Unix-like systems but is not
frequently tested on them.

Notable changes
===============

Denial-of-Service vulnerability
-------------------------------

A denial-of-service vulnerability (CVE-2018-17144) exploitable by miners
has been discovered. IT IS STRONGLY RECOMMENDED to upgrade XPChain Core to
0.17.0-3 immediately.

This bugfix will be enabled by a soft fork (`check_dup_txin`) managed using
BIP9 versionbits. The version bit is bit 3, and nodes will begin tracking
which blocks signal support for the bugfix at the beginning of the first
*bitcoin-ic* retarget period after the bugfix's start date of 1 April 2019.
If 95% of blocks with in a 20,160-block *bitcoin-ic* retarget period
(approx. two weeks) signal support for the bugfix, the soft fork will be locked in.
After another 20,160 blocks, the bugfix will be enabled.

Block signature addition soft fork
----------------------------------

Apart from `check_dup_txin` soft fork, an additional soft fork (`block signature addition`),
tracked on bit 2 of version bit, will be activated. The soft fork will add and check the
signature of block to its coinbase tx, for preventing manipulation of blockchain.

Note that after activation, older clients will **be unable to mint coins** because they
generate a block incompatible to this change.

Bad block rejection
-------------------

Your wallet wrongly accepted invalid blocks to some consensus rules and then it crashed.
Now the wallet rejects them properly, so the crash will never happen.

No more unnecessary minting
---------------------------

The wallet doesn't begin minting before finishing to download blocks from other nodes,
which prevents the wallet from generating invalid blocks that will be rejected.

GUI changes
-----------

 - Better performance on minting tab by transaction nofitication service,
   even when the wallet holds a lot of UTXOs.
 - “Unlock Wallet for Minting Only” menu item gets properly unchecked when you cancel entering passphrase.

XPChain 0.17.0-3 change log
------------------

### Consensus
- #70 `775b417` Consensus: Change consensus.nStakeMinAge (mban259)
- #72 `d89ec2a` Consensus: Reject bad block (mban259)
- #78 `f6d4f63` Consensus: Improve check coinstake (mban259)
- #81 `995ab88` Consensus: Add more checks for duplicated txIns (mban259)
- #82 `5e9e3b3` Consensus:Fix kernel (mban259)
- #84 `cf4540f` Consensus: Undo IsDestinationSame (mban259)

### Mining/Staking
- #62 `e779cc3` minting: check block signature (mban259)
- #79 `66a3bad` Mining: Add IsInitialBlockDownload to BitcoinMinter (mban259)

### P2P
- #73 `03c46e6` Misc: Fix UPnP identifier (serisia)

### GUI
- #65 `bff1496` Qt: Fix problem when cancel unlock (serisia)
- #68 `9d91f40` Qt: Fix startup settings (serisia)
- #69 `b4b914d` Qt: mintingtab implove (serisia)

### RPC
- #74 `a1ee4c9` RPC/REST/ZMQ: Fix binary name (serisia)
- #75 `3fdbe02` RPC/REST/ZMQ: Change minAge value's default in listmintings (serisia)

### Miscellaneous
- #66 `05a3898` Remove bench_bitcoin and test_bitcoin from Docker image (moochannel)
- #67 `6d4baa9` Scripts and tools: Make gen-manpages.sh workable for XPChain (MaySoMusician)
- #71 `d83e155` Misc: Rename thread-name from bitcoin to xpchain (serisia)
- #76 `c23e167` Trivial: rewrite ISSUE_TEMPLATE.md for XPChain (MaySoMusician)

Credits
=======

Thanks to everyone who directly contributed to this release:

- mban259
- moochannel
- serisia
- MaySoMusician

As well as everyone that reported bugs & issues through Discord etc.
