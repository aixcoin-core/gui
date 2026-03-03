# Libraries

| Name                     | Description |
|--------------------------|-------------|
| *libaixcoin_cli*         | RPC client functionality used by *aixcoin-cli* executable |
| *libaixcoin_common*      | Home for common functionality shared by different executables and libraries. Similar to *libaixcoin_util*, but higher-level (see [Dependencies](#dependencies)). |
| *libaixcoin_consensus*   | Consensus functionality used by *libaixcoin_node* and *libaixcoin_wallet*. |
| *libaixcoin_crypto*      | Hardware-optimized functions for data encryption, hashing, message authentication, and key derivation. |
| *libaixcoin_kernel*      | Consensus engine and support library used for validation by *libaixcoin_node*. |
| *libaixcoinqt*           | GUI functionality used by *aixcoin-qt* and *aixcoin-gui* executables. |
| *libaixcoin_ipc*         | IPC functionality used by *aixcoin-node* and *aixcoin-gui* executables to communicate when [`-DENABLE_IPC=ON`](multiprocess.md) is used. |
| *libaixcoin_node*        | P2P and RPC server functionality used by *aixcoind* and *aixcoin-qt* executables. |
| *libaixcoin_util*        | Home for common functionality shared by different executables and libraries. Similar to *libaixcoin_common*, but lower-level (see [Dependencies](#dependencies)). |
| *libaixcoin_wallet*      | Wallet functionality used by *aixcoind* and *aixcoin-wallet* executables. |
| *libaixcoin_wallet_tool* | Lower-level wallet functionality used by *aixcoin-wallet* executable. |
| *libaixcoin_zmq*         | [ZeroMQ](../zmq.md) functionality used by *aixcoind* and *aixcoin-qt* executables. |

## Conventions

- Most libraries are internal libraries and have APIs which are completely unstable! There are few or no restrictions on backwards compatibility or rules about external dependencies. An exception is *libaixcoin_kernel*, which, at some future point, will have a documented external interface.

- Generally each library should have a corresponding source directory and namespace. Source code organization is a work in progress, so it is true that some namespaces are applied inconsistently, and if you look at [`add_library(aixcoin_* ...)`](../../src/CMakeLists.txt) lists you can see that many libraries pull in files from outside their source directory. But when working with libraries, it is good to follow a consistent pattern like:

  - *libaixcoin_node* code lives in `src/node/` in the `node::` namespace
  - *libaixcoin_wallet* code lives in `src/wallet/` in the `wallet::` namespace
  - *libaixcoin_ipc* code lives in `src/ipc/` in the `ipc::` namespace
  - *libaixcoin_util* code lives in `src/util/` in the `util::` namespace
  - *libaixcoin_consensus* code lives in `src/consensus/` in the `Consensus::` namespace

## Dependencies

- Libraries should minimize what other libraries they depend on, and only reference symbols following the arrows shown in the dependency graph below:

<table><tr><td>

```mermaid

%%{ init : { "flowchart" : { "curve" : "basis" }}}%%

graph TD;

aixcoin-cli[aixcoin-cli]-->libaixcoin_cli;

aixcoind[aixcoind]-->libaixcoin_node;
aixcoind[aixcoind]-->libaixcoin_wallet;

aixcoin-qt[aixcoin-qt]-->libaixcoin_node;
aixcoin-qt[aixcoin-qt]-->libaixcoinqt;
aixcoin-qt[aixcoin-qt]-->libaixcoin_wallet;

aixcoin-wallet[aixcoin-wallet]-->libaixcoin_wallet;
aixcoin-wallet[aixcoin-wallet]-->libaixcoin_wallet_tool;

libaixcoin_cli-->libaixcoin_util;
libaixcoin_cli-->libaixcoin_common;

libaixcoin_consensus-->libaixcoin_crypto;

libaixcoin_common-->libaixcoin_consensus;
libaixcoin_common-->libaixcoin_crypto;
libaixcoin_common-->libaixcoin_util;

libaixcoin_kernel-->libaixcoin_consensus;
libaixcoin_kernel-->libaixcoin_crypto;
libaixcoin_kernel-->libaixcoin_util;

libaixcoin_node-->libaixcoin_consensus;
libaixcoin_node-->libaixcoin_crypto;
libaixcoin_node-->libaixcoin_kernel;
libaixcoin_node-->libaixcoin_common;
libaixcoin_node-->libaixcoin_util;

libaixcoinqt-->libaixcoin_common;
libaixcoinqt-->libaixcoin_util;

libaixcoin_util-->libaixcoin_crypto;

libaixcoin_wallet-->libaixcoin_common;
libaixcoin_wallet-->libaixcoin_crypto;
libaixcoin_wallet-->libaixcoin_util;

libaixcoin_wallet_tool-->libaixcoin_wallet;
libaixcoin_wallet_tool-->libaixcoin_util;

classDef bold stroke-width:2px, font-weight:bold, font-size: smaller;
class aixcoin-qt,aixcoind,aixcoin-cli,aixcoin-wallet bold
```
</td></tr><tr><td>

**Dependency graph**. Arrows show linker symbol dependencies. *Crypto* lib depends on nothing. *Util* lib is depended on by everything. *Kernel* lib depends only on consensus, crypto, and util.

</td></tr></table>

- The graph shows what _linker symbols_ (functions and variables) from each library other libraries can call and reference directly, but it is not a call graph. For example, there is no arrow connecting *libaixcoin_wallet* and *libaixcoin_node* libraries, because these libraries are intended to be modular and not depend on each other's internal implementation details. But wallet code is still able to call node code indirectly through the `interfaces::Chain` abstract class in [`interfaces/chain.h`](../../src/interfaces/chain.h) and node code calls wallet code through the `interfaces::ChainClient` and `interfaces::Chain::Notifications` abstract classes in the same file. In general, defining abstract classes in [`src/interfaces/`](../../src/interfaces/) can be a convenient way of avoiding unwanted direct dependencies or circular dependencies between libraries.

- *libaixcoin_crypto* should be a standalone dependency that any library can depend on, and it should not depend on any other libraries itself.

- *libaixcoin_consensus* should only depend on *libaixcoin_crypto*, and all other libraries besides *libaixcoin_crypto* should be allowed to depend on it.

- *libaixcoin_util* should be a standalone dependency that any library can depend on, and it should not depend on other libraries except *libaixcoin_crypto*. It provides basic utilities that fill in gaps in the C++ standard library and provide lightweight abstractions over platform-specific features. Since the util library is distributed with the kernel and is usable by kernel applications, it shouldn't contain functions that external code shouldn't call, like higher level code targeted at the node or wallet. (*libaixcoin_common* is a better place for higher level code, or code that is meant to be used by internal applications only.)

- *libaixcoin_common* is a home for miscellaneous shared code used by different Aixcoin Core applications. It should not depend on anything other than *libaixcoin_util*, *libaixcoin_consensus*, and *libaixcoin_crypto*.

- *libaixcoin_kernel* should only depend on *libaixcoin_util*, *libaixcoin_consensus*, and *libaixcoin_crypto*.

- The only thing that should depend on *libaixcoin_kernel* internally should be *libaixcoin_node*. GUI and wallet libraries *libaixcoinqt* and *libaixcoin_wallet* in particular should not depend on *libaixcoin_kernel* and the unneeded functionality it would pull in, like block validation. To the extent that GUI and wallet code need scripting and signing functionality, they should be able to get it from *libaixcoin_consensus*, *libaixcoin_common*, *libaixcoin_crypto*, and *libaixcoin_util*, instead of *libaixcoin_kernel*.

- GUI, node, and wallet code internal implementations should all be independent of each other, and the *libaixcoinqt*, *libaixcoin_node*, *libaixcoin_wallet* libraries should never reference each other's symbols. They should only call each other through [`src/interfaces/`](../../src/interfaces/) abstract interfaces.

## Work in progress

- Validation code is moving from *libaixcoin_node* to *libaixcoin_kernel* as part of [The libaixcoinkernel Project #27587](https://github.com/aixcoin/aixcoin/issues/27587)
