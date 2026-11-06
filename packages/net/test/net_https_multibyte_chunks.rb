# A TLS read answered a String that read as UTF-8, where CRuby's and IO#sysread's
# are binary. OpenSSL::Buffering's buffer took that encoding and cut by
# characters, so #read and #readpartial handed over more bytes than asked, and a
# chunked response with text in a chunk lost its place at the next chunk's size.
require "net/https"
require "socket"
require "json"

LEAF_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBdDCCARmgAwIBAgICFOUwCgYIKoZIzj0EAwIwHDEaMBgGA1UEAwwRSGFuIFJp
dmVyIFRlc3QgQ0EwIBcNMjYxMDE3MTMxNTUzWhgPMjEyNjA5MjMxMzE1NTNaMCMx
ITAfBgNVBAMMGHR1cm4uamF5YW5nLWRvbmcuZXhhbXBsZTBZMBMGByqGSM49AgEG
CCqGSM49AwEHA0IABLd+fb/6Dh7v0j8wTq9i9pMngYdBanyRG+Gx0hNVWGPdXG6v
lmzEDNShpSzBykTn2fdUaBUldnyOAWzdNprHF2CjQjBAMB0GA1UdDgQWBBRSzkcf
82FjWxf5FScoX08DsF8reTAfBgNVHSMEGDAWgBSjujTEcYqXXjx+n6wC/MwOL5VK
0TAKBggqhkjOPQQDAgNJADBGAiEA9iM/A6HVmXKQL7XDGgjN7f2wiWkZ6calAR65
i16rYbcCIQDjR0XO8BgjrG6mI7C5NevMDdYCuW4qRPD35tzRvssOYw==
-----END CERTIFICATE-----
PEM
KEY_PEM = <<~PEM
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIPIRlQfBz7+HavOzW8H69dntBcUZ0MicF3/FxGctM0W1oAoGCCqGSM49
AwEHoUQDQgAEt359v/oOHu/SPzBOr2L2kyeBh0FqfJEb4bHSE1VYY91cbq+WbMQM
1KGlLMHKROfZ91RoFSV2fI4BbN02mscXYA==
-----END EC PRIVATE KEY-----
PEM

ctx = OpenSSL::SSL::SSLContext.new
ctx.add_certificate(OpenSSL::X509::Certificate.new(LEAF_PEM), OpenSSL::PKey.read(KEY_PEM))
tcp = TCPServer.new("127.0.0.1", 0)
PORT = tcp.addr[1]
server = OpenSSL::SSL::SSLServer.new(tcp, ctx)

PADDING = "{\"x\":127.13,\"y\":37.28}," * 60
TEXT = "{\"msg\":\"정상적으로 처리되었습니다\",\"lines\":[#{PADDING}{\"x\":1}]}"
RAW = TEXT.b
# The first chunk holds all the text; the second cut is inside a character.
WHOLE = TEXT.index("lines") + 400
SPLIT = 9

def framed(cuts)
  out = String.new
  from = 0
  cuts.each do |upto|
    part = RAW[from, upto - from]
    out << part.bytesize.to_s(16) << "\r\n" << part << "\r\n"
    from = upto
  end
  out << "0\r\n\r\n"
  out
end

def head(field)
  "HTTP/1.1 200 OK\r\n#{field}\r\nConnection: close\r\n\r\n".b
end

WIRES = [
  ["chunked", head("Transfer-Encoding: chunked") + framed([WHOLE, WHOLE + 300, RAW.bytesize])],
  ["chunked, a cut inside a character", head("Transfer-Encoding: chunked") + framed([SPLIT, WHOLE, RAW.bytesize])],
  ["sized", head("Content-Length: #{RAW.bytesize}") + RAW]
]
READS = "line one\r\n" + ("a" * 100) + "정상적으로 처리되었습니다" + ("b" * 3000) + "\r\nlast"

client = OpenSSL::SSL::SSLContext.new
client.verify_mode = OpenSSL::SSL::VERIFY_NONE

thread = Thread.new do
  ssl = server.accept
  ssl.write(READS.b)
  ssl.close
  WIRES.each do |_label, wire|
    ssl = server.accept
    while (line = ssl.gets)
      break if line.strip.empty?
    end
    ssl.write(wire)
    ssl.close
  end
end

sock = TCPSocket.new("127.0.0.1", PORT)
ssl = OpenSSL::SSL::SSLSocket.new(sock, client)
ssl.connect
line = ssl.gets
puts "gets: #{line.bytesize} #{line.encoding}"
part = ssl.readpartial(120)
puts "readpartial: #{part.bytesize} #{part.encoding}"
exact = ssl.read(50)
puts "read: #{exact.bytesize} #{exact.encoding}"
rest = ssl.read
puts "the rest: #{rest.bytesize} #{(line + part + exact + rest) == READS.b}"
ssl.close
sock.close

WIRES.each do |label, _wire|
  http = Net::HTTP.new("127.0.0.1", PORT)
  http.use_ssl = true
  http.verify_mode = OpenSSL::SSL::VERIFY_NONE
  res = http.get("/x")
  body = res.body.to_s
  puts "#{label}: #{res.code} #{body.bytesize == RAW.bytesize} #{body.b == RAW}"
  puts JSON.parse(body)["msg"]
end
thread.join
