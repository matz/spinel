# A connection accepted from a UNIXServer is a UNIXSocket, not a TCPSocket
# (#11346)
require "socket"
path = "/tmp/spinel_unix_#{Process.pid}.sock"
File.delete(path) if File.exist?(path)
server = UNIXServer.new(path)
client = UNIXSocket.new(path)
conn = server.accept
p conn.class
p conn.is_a?(TCPSocket)
p conn.is_a?(UNIXSocket)
client.write("hi\n")
p conn.gets
client.close
conn.close
server.close
File.delete(path)
