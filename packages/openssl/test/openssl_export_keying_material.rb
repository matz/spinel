# SSLSocket#export_keying_material, the RFC 5705 / RFC 8446 exporter that
# DTLS-SRTP and channel binding key from, and #peer_cert. Each case is a
# handshake on loopback, a server thread and a client in this one: both ends
# export with the same label, length and context, and what they get is
# compared, never printed, as it differs on every run.
require "openssl"
require "socket"

# The certificate for turn.jayang-dong.example and its key, the Han River
# Test CA that issued it, and a client certificate for relay.gimpo.example
# from the same CA: the ones openssl_server_verify.rb uses, made for these
# tests and valid until 2126.
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
CA_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBjzCCATWgAwIBAgIUI9ic9bX/0AmUyUWTCXiglCPfR0UwCgYIKoZIzj0EAwIw
HDEaMBgGA1UEAwwRSGFuIFJpdmVyIFRlc3QgQ0EwIBcNMjYxMDE3MTMxNTUzWhgP
MjEyNjA5MjMxMzE1NTNaMBwxGjAYBgNVBAMMEUhhbiBSaXZlciBUZXN0IENBMFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEE4L0e4gJjpuc16X4tfwznrmlc4D+z3vQ
fGOiB2cJwwEhNaWDNk95IGW5n5WF64ObqEYcFpokFyWur1o4maXi1KNTMFEwHQYD
VR0OBBYEFKO6NMRxipdePH6frAL8zA4vlUrRMB8GA1UdIwQYMBaAFKO6NMRxipde
PH6frAL8zA4vlUrRMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIg
XYulJlIdEJaj58GW085QwX6x3q2emzlns3++66sg1bsCIQCx71OOJjZW1fB6A8vh
c9tqElaq1Z6KVZ3MlZ1IgrVQpQ==
-----END CERTIFICATE-----
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
CA = OpenSSL::X509::Certificate.new(CA_PEM)
CLIENT = OpenSSL::X509::Certificate.new(CLIENT_PEM)
CLIENT_KEY = OpenSSL::PKey.read(CLIENT_KEY_PEM)

LABEL = "EXTRACTOR-dtls_srtp"

# What one end exports: the same requests on either side, in this order.
def exports(ssl)
  [
    ssl.export_keying_material(LABEL, 60),
    ssl.export_keying_material(LABEL, 60, nil),
    ssl.export_keying_material(LABEL, 60, ""),
    ssl.export_keying_material(LABEL, 60, "context"),
    ssl.export_keying_material("EXPORTER-Channel-Binding", 60),
    ssl.export_keying_material(LABEL, 32),
  ]
end

# One connection. The client does not verify the server. When client_cert
# is set the server asks for a certificate, checks it against the CA, and
# the client sends one.
def connect(client_cert)
  server = TCPServer.new("127.0.0.1", 0)
  port = server.addr[1]
  sctx = OpenSSL::SSL::SSLContext.new
  sctx.add_certificate(LEAF, KEY)
  t = Thread.new do
    sock = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(sock, sctx)
    ssl.accept
    line = ssl.gets
    out = [exports(ssl), ssl.peer_cert]
    ssl.write("ok #{line}")
    ssl.close
    sock.close
    out
  end

  ctx = OpenSSL::SSL::SSLContext.new
  ctx.verify_mode = OpenSSL::SSL::VERIFY_NONE
  if client_cert
    sctx.verify_mode = OpenSSL::SSL::VERIFY_PEER
    store = OpenSSL::X509::Store.new
    store.add_cert(CA)
    sctx.cert_store = store
    ctx.cert = CLIENT
    ctx.key = CLIENT_KEY
  end
  sock = TCPSocket.new("127.0.0.1", port)
  ssl = OpenSSL::SSL::SSLSocket.new(sock, ctx)
  before = ssl.peer_cert
  ssl.connect
  mine = exports(ssl)
  cert = ssl.peer_cert
  ssl.write("Incheon\n")
  reply = ssl.gets
  ssl.close
  sock.close
  theirs, server_sees = t.value
  server.close
  [before, mine, cert, theirs, server_sees, reply]
end

before, mine, cert, theirs, server_sees, reply = connect(false)
p reply
p before
# Each answer is a binary String of the length asked for.
p mine.map(&:bytesize)
p mine.map { |s| s.encoding }.uniq
# The two ends agree on every request.
p mine == theirs
# No context and an empty one are the same request under TLS 1.3 (RFC 8446
# 7.5), which the handshake here negotiates; a context, another label or
# another length is another answer.
p [mine[0] == mine[1], mine[1] == mine[2], mine[0] == mine[3], mine[0] == mine[4], mine[0].byteslice(0, 32) == mine[5]]
# The client's view of the server's certificate is the one the server has.
p cert.class
p cert.to_der == LEAF.to_der
# A client sent no certificate, so the server has none to show.
p server_sees

_, mine, _, theirs, server_sees, reply = connect(true)
p reply
p mine == theirs
p server_sees.to_der == CLIENT.to_der

# Before a handshake there is nothing to export from.
server = TCPServer.new("127.0.0.1", 0)
sock = TCPSocket.new("127.0.0.1", server.addr[1])
ssl = OpenSSL::SSL::SSLSocket.new(sock)
begin
  ssl.export_keying_material(LABEL, 60)
rescue OpenSSL::SSL::SSLError => e
  p [e.class, e.message]
end
sock.close
server.close
