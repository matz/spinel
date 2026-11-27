# SSLContext#min_version= and #max_version=, as CRuby takes them: an Integer
# protocol constant, a Symbol or String naming one, or nil for no bound.
# Each handshake is on loopback, a server thread and a client in this one,
# and reports the version both ends agreed on, or that the handshake failed.
require "openssl"
require "socket"

# The certificate for turn.jayang-dong.example and its key: the pair
# openssl_server_accept.rb uses, made for these tests and valid until 2126.
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

LEAF = OpenSSL::X509::Certificate.new(LEAF_PEM)
KEY = OpenSSL::PKey.read(KEY_PEM)

def server_ctx
  ctx = OpenSSL::SSL::SSLContext.new
  ctx.add_certificate(LEAF, KEY)
  ctx
end

def client_ctx
  ctx = OpenSSL::SSL::SSLContext.new
  ctx.verify_mode = OpenSSL::SSL::VERIFY_NONE
  ctx
end

# One connection: the version each end reports, or "refused" for an end
# whose handshake failed.
def exchange(sctx, cctx)
  server = TCPServer.new("127.0.0.1", 0)
  port = server.addr[1]
  t = Thread.new do
    sock = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(sock, sctx)
    begin
      ssl.accept
      line = ssl.gets
      ssl.write(line)
      version = ssl.ssl_version
      ssl.close
      version
    rescue OpenSSL::SSL::SSLError, SystemCallError
      "refused"
    ensure
      sock.close
    end
  end

  sock = TCPSocket.new("127.0.0.1", port)
  ssl = OpenSSL::SSL::SSLSocket.new(sock, cctx)
  mine = nil
  begin
    ssl.connect
    ssl.write("Incheon\n")
    ssl.gets
    mine = ssl.ssl_version
  rescue OpenSSL::SSL::SSLError, SystemCallError
    mine = "refused"
  ensure
    ssl.close
  end
  sock.close
  p [mine, t.value]
  server.close
end

p [OpenSSL::SSL::SSL2_VERSION, OpenSSL::SSL::SSL3_VERSION, OpenSSL::SSL::TLS1_VERSION,
   OpenSSL::SSL::TLS1_1_VERSION, OpenSSL::SSL::TLS1_2_VERSION, OpenSSL::SSL::TLS1_3_VERSION]

# No bound on either end: the newest both have.
exchange(server_ctx, client_ctx)

# A server bounded at TLS 1.2, by its constant, and a client left alone.
sctx = server_ctx
sctx.max_version = OpenSSL::SSL::TLS1_2_VERSION
exchange(sctx, client_ctx)

# A client bounded the same way, by name.
cctx = client_ctx
cctx.max_version = :TLS1_2
exchange(server_ctx, cctx)

# Ends whose ranges do not meet: neither handshake completes.
cctx = client_ctx
cctx.min_version = OpenSSL::SSL::TLS1_3_VERSION
sctx = server_ctx
sctx.max_version = "TLS1_2"
exchange(sctx, cctx)

sctx = server_ctx
sctx.min_version = :TLS1_3
cctx = client_ctx
cctx.max_version = :TLS1_2
exchange(sctx, cctx)

# nil takes a bound off again. (A context a connection has used is frozen
# in CRuby, so this is a fresh one.)
sctx = server_ctx
sctx.min_version = :TLS1_3
cctx = client_ctx
cctx.max_version = :TLS1_2
cctx.max_version = nil
exchange(sctx, cctx)

# A name is matched as CRuby matches it, by its first letters: TLS is TLS1.
ctx = client_ctx
ctx.min_version = :TLS
p :prefix

# What CRuby refuses, it refuses the same way: a name it does not know, a
# number OpenSSL takes for no version, and something that is no name.
[:TLS9, "nope", 12345, "", 1.5, Object.new].each do |v|
  ctx = client_ctx
  begin
    ctx.max_version = v
    p [v.class, :set]
  rescue ArgumentError, TypeError, OpenSSL::SSL::SSLError => e
    p [v.class, e.class, e.message]
  end
end

# A bound that fails to set leaves the one before it in place.
cctx = client_ctx
cctx.max_version = :TLS1_2
begin
  cctx.max_version = 12345
rescue OpenSSL::SSL::SSLError => e
  p e.class
end
exchange(server_ctx, cctx)
