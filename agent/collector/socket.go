package collector

import (
	"agent/network"
	"strconv"
	"strings"
	"sync"

	"github.com/prometheus/procfs"
	"golang.org/x/sys/unix"
)

var (
	nlSocketContext     *network.Context
	nlSocketSingleton   *network.VNetlink
	nlSocketContextOnce sync.Once
	nlSocketOnce        sync.Once
)

func GetNlSocketContext() *network.Context {
	nlSocketContextOnce.Do(func() {
		nlSocketContext = &network.Context{}
	})
	return nlSocketContext
}

func GetNlSocketSingleton() *network.VNetlink {
	nlSocketOnce.Do(func() {
		nlSocketSingleton = &network.VNetlink{}
	})
	return nlSocketSingleton
}

// netlink 方式获取
func GetSockets(disableProc bool, status uint8) (sockets []network.Socket, err error) {
	var udpSockets, udp6Sockets, tcpSockets, tcp6Sockets []network.Socket
	ctx := GetNlSocketContext()
	nlsocket := GetNlSocketSingleton()
	// 先初始化协议
	nlsocket.Protocal = unix.NETLINK_INET_DIAG
	if err = ctx.IRetry(nlsocket); err != nil {
		return
	}

	if status != network.TCP_ESTABLISHED {
		if udpSockets, err = nlsocket.GetSockets(unix.AF_INET, unix.IPPROTO_UDP, status); err != nil {
			return
		}
		sockets = append(sockets, udpSockets...)
		udp6Sockets, err = nlsocket.GetSockets(unix.AF_INET6, unix.IPPROTO_UDP, status)
		if err == nil {
			sockets = append(sockets, udp6Sockets...)
		}
	}

	tcpSockets, err = nlsocket.GetSockets(unix.AF_INET, unix.IPPROTO_TCP, status)
	if err == nil {
		sockets = append(sockets, tcpSockets...)
	}
	tcp6Sockets, err = nlsocket.GetSockets(unix.AF_INET6, unix.IPPROTO_TCP, status)
	if err == nil {
		sockets = append(sockets, tcp6Sockets...)
	}

	inodeMap := make(map[uint32]int)
	for index, socket := range sockets {
		if socket.Inode != 0 {
			inodeMap[socket.Inode] = index
		}
	}
	if !disableProc {
		procs, err := procfs.AllProcs()
		if err == nil {
			for _, p := range procs {
				fds, _ := p.FileDescriptorTargets()
				for _, fd := range fds {
					if strings.HasPrefix(fd, "socket:[") {
						inode, _ := strconv.ParseUint(strings.TrimRight(fd[8:], "]"), 10, 32)
						index, ok := inodeMap[uint32(inode)]
						if ok {
							sockets[index].PID = int32(p.PID)
							sockets[index].Comm, _ = p.Comm()
							argv, err := p.CmdLine()
							if err == nil {
								if len(argv) > 16 {
									argv = argv[:16]
								}
								sockets[index].Argv = strings.Join(argv, " ")
								if len(sockets[index].Argv) > 32 {
									sockets[index].Argv = sockets[index].Argv[:32]
								}
							}
						}
					}
				}
			}
		}
	}
	return
}
