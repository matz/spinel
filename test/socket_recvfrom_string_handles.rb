# spinel: gc-minor
# A recvfrom row holds the payload and address Strings it returns.
require "socket"

receiver = UDPSocket.new
receiver.bind("127.0.0.1", 0)
sender = UDPSocket.new
port = receiver.addr[1]
sender.send("\xff\0".b, 0, "127.0.0.1", port)
row = receiver.recvfrom(16)
data = row[0]
host = row[1][2]
p [data.encoding.name, data.bytes, row[1][0].frozen?, host.equal?(row[1][3])]
data << "x".b * 256
host << "!" * 256
p [row[0].bytesize, row[1][2].bytesize, row[1][3].bytesize]
sender.send("x", 0, "127.0.0.1", port)
empty = receiver.recvfrom(0)
p [empty[0].encoding.name, empty[0].bytes]
receiver.close
sender.close
