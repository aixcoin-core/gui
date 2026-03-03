Sample init scripts and service configuration for aixcoind
==========================================================

Sample scripts and configuration files for systemd, Upstart and OpenRC
can be found in the contrib/init folder.

    contrib/init/aixcoind.service:    systemd service unit configuration
    contrib/init/aixcoind.openrc:     OpenRC compatible SysV style init script
    contrib/init/aixcoind.openrcconf: OpenRC conf.d file
    contrib/init/aixcoind.conf:       Upstart service configuration file
    contrib/init/aixcoind.init:       CentOS compatible SysV style init script

Service User
---------------------------------

All three Linux startup configurations assume the existence of a "aixcoin" user
and group.  They must be created before attempting to use these scripts.
The macOS configuration assumes aixcoind will be set up for the current user.

Configuration
---------------------------------

Running aixcoind as a daemon does not require any manual configuration. You may
set the `rpcauth` setting in the `aixcoin.conf` configuration file to override
the default behaviour of using a special cookie for authentication.

This password does not have to be remembered or typed as it is mostly used
as a fixed token that aixcoind and client programs read from the configuration
file, however it is recommended that a strong and secure password be used
as this password is security critical to securing the wallet should the
wallet be enabled.

If aixcoind is run with the "-server" flag (set by default), and no rpcpassword is set,
it will use a special cookie file for authentication. The cookie is generated with random
content when the daemon starts, and deleted when it exits. Read access to this file
controls who can access it through RPC.

By default the cookie is stored in the data directory, but its location can be
overridden with the option `-rpccookiefile`. Default file permissions for the
cookie are "owner" (i.e. user read/writeable) via default application-wide file
umask of `0077`, but these can be overridden with the `-rpccookieperms` option.

This allows for running aixcoind without having to do any manual configuration.

`conf`, `pid`, and `wallet` accept relative paths which are interpreted as
relative to the data directory. `wallet` *only* supports relative paths.

To generate an example configuration file that describes the configuration settings,
see [contrib/devtools/README.md](../contrib/devtools/README.md#gen-aixcoin-confsh).

Paths
---------------------------------

### Linux

All three configurations assume several paths that might need to be adjusted.

    Binary:              /usr/bin/aixcoind
    Configuration file:  /etc/aixcoin/aixcoin.conf
    Data directory:      /var/lib/aixcoind
    PID file:            /var/run/aixcoind/aixcoind.pid (OpenRC and Upstart) or
                         /run/aixcoind/aixcoind.pid (systemd)
    Lock file:           /var/lock/subsys/aixcoind (CentOS)

The PID directory (if applicable) and data directory should both be owned by the
aixcoin user and group. It is advised for security reasons to make the
configuration file and data directory only readable by the aixcoin user and
group. Access to aixcoin-cli and other aixcoind rpc clients can then be
controlled by group membership.

NOTE: When using the systemd .service file, the creation of the aforementioned
directories and the setting of their permissions is automatically handled by
systemd. Directories are given a permission of 710, giving the aixcoin group
access to files under it _if_ the files themselves give permission to the
aixcoin group to do so. This does not allow
for the listing of files under the directory.

NOTE: It is not currently possible to override `datadir` in
`/etc/aixcoin/aixcoin.conf` with the current systemd, OpenRC, and Upstart init
files out-of-the-box. This is because the command line options specified in the
init files take precedence over the configurations in
`/etc/aixcoin/aixcoin.conf`. However, some init systems have their own
configuration mechanisms that would allow for overriding the command line
options specified in the init files (e.g. setting `BITCOIND_DATADIR` for
OpenRC).

### macOS

    Binary:              /usr/local/bin/aixcoind
    Configuration file:  ~/Library/Application Support/Aixcoin/aixcoin.conf
    Data directory:      ~/Library/Application Support/Aixcoin
    Lock file:           ~/Library/Application Support/Aixcoin/.lock

Installing Service Configuration
-----------------------------------

### systemd

Installing this .service file consists of just copying it to
/usr/lib/systemd/system directory, followed by the command
`systemctl daemon-reload` in order to update running systemd configuration.

To test, run `systemctl start aixcoind` and to enable for system startup run
`systemctl enable aixcoind`

NOTE: When installing for systemd in Debian/Ubuntu the .service file needs to be copied to the /lib/systemd/system directory instead.

### OpenRC

Rename aixcoind.openrc to aixcoind and drop it in /etc/init.d.  Double
check ownership and permissions and make it executable.  Test it with
`/etc/init.d/aixcoind start` and configure it to run on startup with
`rc-update add aixcoind`

### Upstart (for Debian/Ubuntu based distributions)

Upstart is the default init system for Debian/Ubuntu versions older than 15.04. If you are using version 15.04 or newer and haven't manually configured upstart you should follow the systemd instructions instead.

Drop aixcoind.conf in /etc/init.  Test by running `service aixcoind start`
it will automatically start on reboot.

NOTE: This script is incompatible with CentOS 5 and Amazon Linux 2014 as they
use old versions of Upstart and do not supply the start-stop-daemon utility.

### CentOS

Copy aixcoind.init to /etc/init.d/aixcoind. Test by running `service aixcoind start`.

Using this script, you can adjust the path and flags to the aixcoind program by
setting the BITCOIND and FLAGS environment variables in the file
/etc/sysconfig/aixcoind. You can also use the DAEMONOPTS environment variable here.

### macOS

Copy org.aixcoin.aixcoind.plist into ~/Library/LaunchAgents. Load the launch agent by
running `launchctl load ~/Library/LaunchAgents/org.aixcoin.aixcoind.plist`.

This Launch Agent will cause aixcoind to start whenever the user logs in.

NOTE: This approach is intended for those wanting to run aixcoind as the current user.
You will need to modify org.aixcoin.aixcoind.plist if you intend to use it as a
Launch Daemon with a dedicated aixcoin user.

Auto-respawn
-----------------------------------

Auto respawning is currently only configured for Upstart and systemd.
Reasonable defaults have been chosen but YMMV.
