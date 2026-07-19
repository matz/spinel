require "socket"
# Core address-family / socket-type / protocol / option constants must match
# the CRuby integer values (source: ext/socket/constants.c / mkconstants.rb).
p Socket::AF_INET
p Socket::AF_INET6
p Socket::AF_UNIX
p Socket::SOCK_STREAM
p Socket::SOCK_DGRAM
p Socket::SOCK_RAW
p Socket::IPPROTO_TCP
p Socket::IPPROTO_UDP
p Socket::SOL_SOCKET
p Socket::SO_REUSEADDR
p Socket::SO_TYPE
p Socket::MSG_OOB
p Socket::MSG_PEEK
p Socket::SHUT_RD
p Socket::SHUT_WR
p Socket::SHUT_RDWR
p Socket::SOMAXCONN
p Socket::AI_PASSIVE
p Socket::AI_CANONNAME
p Socket::EAI_NONAME
