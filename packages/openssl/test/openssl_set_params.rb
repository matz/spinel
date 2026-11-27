# SSLContext#set_params reads verify_mode and verify_hostname from its
# params, over CRuby's defaults. Net::SMTP's tls_verify: false hands it
# verify_mode: VERIFY_NONE, and a client then connects without verifying; a
# server given VERIFY_NONE asks for no client certificate. min_version and
# max_version go to their writers, so a handshake keeps to them.
require "openssl"
require "socket"

# The certificate for turn.jayang-dong.example and its key, from the Han
# River Test CA, which no system store trusts, and the client certificate of
# openssl_server_verify.rb.
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
CLIENT_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBbzCCARSgAwIBAgICDZYwCgYIKoZIzj0EAwIwHDEaMBgGA1UEAwwRSGFuIFJp
dmVyIFRlc3QgQ0EwIBcNMjYxMDE4MDc0NDM5WhgPMjEyNjA5MjQwNzQ0MzlaMB4x
HDAaBgNVBAMME3JlbGF5LmdpbXBvLmV4YW1wbGUwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAASYNQdzYEW3UG5uTdNaNXnxgnYimjQ3EPyCazKBizPI1/sa83RYiAbi
iLJwA/nhdNsD7UWdOGmbMUHS3Yq5BPlGo0IwQDAdBgNVHQ4EFgQUSxATrJm3eVZB
Y9gKJOJOgMKMFwYwHwYDVR0jBBgwFoAUo7o0xHGKl148fp+sAvzMDi+VStEwCgYI
KoZIzj0EAwIDSQAwRgIhAO8vpRDua2ztaC8/svak1/7J1dY6Sc7w/zhPOkrKMvXH
AiEAlR5zo5FldeQdmTv8xMkUMApKwgT5BkJ9zTy3o6Ost7A=
-----END CERTIFICATE-----
PEM
CLIENT_KEY_PEM = <<~PEM
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIC6Le/KNXBgrpXVJKO9xa+8zyfT3yzeh5l/v0wAjkJqdoAoGCCqGSM49
AwEHoUQDQgAEmDUHc2BFt1Bubk3TWjV58YJ2Ipo0NxD8gmsygYszyNf7GvN0WIgG
4oiycAP54XTbA+1FnThpmzFB0t2KuQT5Rg==
-----END EC PRIVATE KEY-----
PEM

LEAF = OpenSSL::X509::Certificate.new(LEAF_PEM)
KEY = OpenSSL::PKey.read(KEY_PEM)
CLIENT = OpenSSL::X509::Certificate.new(CLIENT_PEM)
CLIENT_KEY = OpenSSL::PKey.read(CLIENT_KEY_PEM)

def settings(params)
  ctx = OpenSSL::SSL::SSLContext.new
  ctx.set_params(params)
  [ctx.verify_mode, ctx.verify_hostname]
end

p settings({})
p settings(verify_mode: OpenSSL::SSL::VERIFY_NONE)
p settings(verify_hostname: false)
p settings(verify_mode: OpenSSL::SSL::VERIFY_PEER, verify_hostname: true)

# One connection; each end reports only whether it got through.
def exchange(server_ctx, client_ctx)
  server = TCPServer.new("127.0.0.1", 0)
  port = server.addr[1]
  t = Thread.new do
    sock = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(sock, server_ctx)
    begin
      ssl.accept
      ssl.write("Han River: #{ssl.gets}")
      ssl.close
      "accepted"
    rescue OpenSSL::SSL::SSLError, SystemCallError
      "refused"
    ensure
      sock.close
    end
  end
  sock = TCPSocket.new("127.0.0.1", port)
  ssl = OpenSSL::SSL::SSLSocket.new(sock, client_ctx)
  ssl.hostname = "turn.jayang-dong.example"
  reply = nil
  begin
    ssl.connect
    ssl.write("Gimpo\n")
    reply = ssl.gets
  rescue OpenSSL::SSL::SSLError, SystemCallError
    reply = nil
  ensure
    ssl.close
  end
  sock.close
  p [reply.nil? ? "no reply" : reply, t.value]
  server.close
end

# CRuby freezes a context at its first handshake, so each server is new.
def server(params)
  ctx = OpenSSL::SSL::SSLContext.new
  ctx.add_certificate(LEAF, KEY)
  ctx.set_params(params) unless params.nil?
  ctx
end

# A client given VERIFY_NONE connects to a server whose CA it does not
# trust; with the defaults it refuses that server.
client = OpenSSL::SSL::SSLContext.new
client.set_params(verify_mode: OpenSSL::SSL::VERIFY_NONE)
exchange(server(nil), client)
client = OpenSSL::SSL::SSLContext.new
client.set_params({})
exchange(server(nil), client)

# A server given VERIFY_NONE asks for no certificate, so a client's is not
# checked against the server's empty store; with the defaults it is, and
# refused.
client = OpenSSL::SSL::SSLContext.new
client.verify_mode = OpenSSL::SSL::VERIFY_NONE
client.cert = CLIENT
client.key = CLIENT_KEY
exchange(server(verify_mode: OpenSSL::SSL::VERIFY_NONE), client)
exchange(server({}), client)

# The version a server and a client made from these params agree on, or
# that they agree on none.
def version_between(server_params, client_params)
  sctx = OpenSSL::SSL::SSLContext.new
  sctx.add_certificate(LEAF, KEY)
  sctx.set_params(server_params)
  cctx = OpenSSL::SSL::SSLContext.new
  cctx.set_params(client_params)
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    sock = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(sock, sctx)
    begin
      ssl.accept
      ssl.close
    rescue OpenSSL::SSL::SSLError, SystemCallError
    ensure
      sock.close
    end
  end
  sock = TCPSocket.new("127.0.0.1", server.addr[1])
  ssl = OpenSSL::SSL::SSLSocket.new(sock, cctx)
  version = "none"
  begin
    ssl.connect
    version = ssl.ssl_version
  rescue OpenSSL::SSL::SSLError, SystemCallError
  ensure
    ssl.close
  end
  sock.close
  t.join
  server.close
  version
end

none = OpenSSL::SSL::VERIFY_NONE
p version_between({ verify_mode: none, max_version: OpenSSL::SSL::TLS1_2_VERSION }, { verify_mode: none })
p version_between({ verify_mode: none }, { verify_mode: none, max_version: :TLS1_2 })
p version_between({ verify_mode: none, min_version: :TLS1_3 }, { verify_mode: none, max_version: :TLS1_2 })
p version_between({ verify_mode: none, min_version: :TLS1_2, max_version: :TLS1_2 },
                  { verify_mode: none, min_version: :TLS1_2 })

# An unknown version raises from set_params as from the writer, after the
# keys before it are set.
ctx = OpenSSL::SSL::SSLContext.new
begin
  ctx.set_params(verify_mode: none, min_version: :TLS1_9)
rescue ArgumentError => e
  p e.message
end
p ctx.verify_mode
