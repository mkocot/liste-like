# listen-like

See for argument description: https://www.freedesktop.org/software/systemd/man/systemd.socket.html

Mimicking interface: https://www.freedesktop.org/software/systemd/man/sd_listen_fds.html#

## Usage
```
Usage: listen-like [OPTION...] -- APP_TO_RUN [args]
Run me like your fancy systemd

      --Backlog=BACKLOG
      --DirectoryMode=MODE
      --IPDSCP=DSCP
      --IPTOS=TOS            Deprecated. Use --IPDSCP.
      --IPTTL=TTL
      --KeepAlive
      --KeepAliveIntervalSec=SEC
      --KeepAliveProbes=N
      --KeepAliveTimeSec=SEC
      --ListenDatagram=DATAGRAM
      --ListenSequentialPacket=SEQ
      --ListenStream=STREAM
      --LockUnixSockets      Will create $path/~$socket lock file and pass FD
                             to executed process. If eg. $LISTEN_FDS=1, then
                             lock socket will have assigned FD=3+$LISTEN_FDS,
                             FD=3+$LISTEN_FDS + 1, ... and so on. This is
                             implementation detail you SHOULD NOT use in
                             production, but might be quirky enough when you
                             start investigating: `why my fd has been assigned
                             so big number and not XYZ`
      --Mark=MARK
      --Priority=PRIORITY
      --ReceiveBuffer=BYTES
      --ReuseAddress
      --ReusePort
      --SendBuffer=BYTES
      --SocketGroup=GROUP
      --SocketMode=MODE
      --SocketProtocol=PROT
      --SocketUser=USER
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version
```

## Example
Open 2 terminals:
1. ```
   ./listen-like --ListenStream=127.0.0.1:1025 -- test
      App to run: test
      Arguments: test 
      Listening: 127.0.0.1:1025
      ACTIVE FD=3
      Active fd=0
      Active fd=1
      Active fd=2
      Active fd=3
      sockets_from_systemd=1
   ```
2. ```
   nc 127.0.0.1 1025
   HelloWorld
   ```

   
