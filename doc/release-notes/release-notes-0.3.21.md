Binaries for Aix version 0.3.21 are available at:
  https://sourceforge.net/projects/aix/files/Aix/aix-0.3.21/

Changes and new features from the 0.3.20 release include:

* Universal Plug and Play support.  Enable automatic opening of a port for incoming connections by running aix or aixd with the - -upnp=1 command line switch or using the Options dialog box.

* Support for full-precision aix amounts.  You can now send, and aix will display, aix amounts smaller than 0.01.  However, sending fewer than 0.01 aixs still requires a 0.01 aix fee (so you can send 1.0001 aixs without a fee, but you will be asked to pay a fee if you try to send 0.0001).

* A new method of finding aix nodes to connect with, via DNS A records. Use the -dnsseed option to enable.

For developers, changes to aix's remote-procedure-call API:

* New rpc command "sendmany" to send aixs to more than one address in a single transaction.

* Several bug fixes, including a serious intermittent bug that would sometimes cause aixd to stop accepting rpc requests. 

* -logtimestamps option, to add a timestamp to each line in debug.log.

* Immature blocks (newly generated, under 120 confirmations) are now shown in listtransactions.
