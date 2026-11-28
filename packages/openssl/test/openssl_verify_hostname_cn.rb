# A host name against the subject's CN, as CRuby's
# OpenSSL::SSL.verify_certificate_identity does it: the CN counts only for a
# certificate whose subjectAltName names no DNS name and no IP address. The
# three certificates below carry the same CN and differ in that extension:
# none, a DNS name, an IP address. OpenSSL 4 stopped looking at the CN on its
# own, and 3.x looked at it beside an IP address, so each is checked here.
require "openssl"
require "socket"

# Issued by the Mangwon Test CA to pier.mangwon.example, all three on one
# P-256 key.
NO_SAN_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBbzCCARWgAwIBAgIES1iR5DAKBggqhkjOPQQDAjAaMRgwFgYDVQQDDA9NYW5n
d29uIFRlc3QgQ0EwIBcNMjYxMTI4MDE1NjE4WhgPMjEyNjExMDQwMTU2MThaMB8x
HTAbBgNVBAMMFHBpZXIubWFuZ3dvbi5leGFtcGxlMFkwEwYHKoZIzj0CAQYIKoZI
zj0DAQcDQgAEEofaees11hLk0PfT21h/baPn5P0xGVyioVQ4AJSVW7Pt0w5TOF62
Skd/55wUgRnkCMYdQq8zI7M2H6TlsTmnh6NCMEAwHQYDVR0OBBYEFJczvoZ97ju5
eCIYOHCrwqxeGn2pMB8GA1UdIwQYMBaAFG6UnzKiObNU6RRZa5jVZ855ywQaMAoG
CCqGSM49BAMCA0gAMEUCIEsg5X1vp2+2J/ekMuSwvuAbIZazct8FL5vAJcVk8TCb
AiEAxsIShc1vFmjKrVA+9gyX/dVV2Wyfm8zpIm3kvU4gbOw=
-----END CERTIFICATE-----
PEM
DNS_SAN_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBkjCCATigAwIBAgIFAOO8l2owCgYIKoZIzj0EAwIwGjEYMBYGA1UEAwwPTWFu
Z3dvbiBUZXN0IENBMCAXDTI2MTEyODAxNTYxOFoYDzIxMjYxMTA0MDE1NjE4WjAf
MR0wGwYDVQQDDBRwaWVyLm1hbmd3b24uZXhhbXBsZTBZMBMGByqGSM49AgEGCCqG
SM49AwEHA0IABBKH2nnrNdYS5ND309tYf22j5+T9MRlcoqFUOACUlVuz7dMOUzhe
tkpHf+ecFIEZ5AjGHUKvMyOzNh+k5bE5p4ejZDBiMB0GA1UdDgQWBBSXM76Gfe47
uXgiGDhwq8KsXhp9qTAfBgNVHSMEGDAWgBRulJ8yojmzVOkUWWuY1WfOecsEGjAg
BgNVHREEGTAXghVmZXJyeS5tYW5nd29uLmV4YW1wbGUwCgYIKoZIzj0EAwIDSAAw
RQIhAImzdK1cN0Pnufs5SMBjQAPbOg6dhFzEy57NXWxrmy93AiB5aCXPf1aAs0JC
8v/+g7ffToHe4JdCGWIBP4abD1mzIw==
-----END CERTIFICATE-----
PEM
IP_SAN_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBgTCCASagAwIBAgIEJ6Oo6zAKBggqhkjOPQQDAjAaMRgwFgYDVQQDDA9NYW5n
d29uIFRlc3QgQ0EwIBcNMjYxMTI4MDE1NjE4WhgPMjEyNjExMDQwMTU2MThaMB8x
HTAbBgNVBAMMFHBpZXIubWFuZ3dvbi5leGFtcGxlMFkwEwYHKoZIzj0CAQYIKoZI
zj0DAQcDQgAEEofaees11hLk0PfT21h/baPn5P0xGVyioVQ4AJSVW7Pt0w5TOF62
Skd/55wUgRnkCMYdQq8zI7M2H6TlsTmnh6NTMFEwHQYDVR0OBBYEFJczvoZ97ju5
eCIYOHCrwqxeGn2pMB8GA1UdIwQYMBaAFG6UnzKiObNU6RRZa5jVZ855ywQaMA8G
A1UdEQQIMAaHBMAAAgcwCgYIKoZIzj0EAwIDSQAwRgIhALaovtYqrQ43/NUF0wcC
MOpOiaNR8koaq8L0TKV9pXL/AiEAiCpKLROoifZIgC+q40pC8/l5zhitEjzJo7o5
4cul/+Y=
-----END CERTIFICATE-----
PEM
KEY_PEM = <<~PEM
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIC81EOA7r0sh2KxQinq92nuFEY1Wh0OkmvUt3EXT9WyMoAoGCCqGSM49
AwEHoUQDQgAEEofaees11hLk0PfT21h/baPn5P0xGVyioVQ4AJSVW7Pt0w5TOF62
Skd/55wUgRnkCMYdQq8zI7M2H6TlsTmnhw==
-----END EC PRIVATE KEY-----
PEM
CA_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBazCCARCgAwIBAgIUb7uHBGKZtcBEtte86Cl5GxOoLtIwCgYIKoZIzj0EAwIw
GjEYMBYGA1UEAwwPTWFuZ3dvbiBUZXN0IENBMCAXDTI2MTEyODAxNTYxOFoYDzIx
MjYxMTA0MDE1NjE4WjAaMRgwFgYDVQQDDA9NYW5nd29uIFRlc3QgQ0EwWTATBgcq
hkjOPQIBBggqhkjOPQMBBwNCAASJdRtVbpa1C7OCv3HIOAA3FHsfGrKea/hyOuf/
ZGtQFjXIDBD8fCVvEwsYXPnmtAJD2B+Jl9dLpAU0pv7BXWz9ozIwMDAPBgNVHRMB
Af8EBTADAQH/MB0GA1UdDgQWBBRulJ8yojmzVOkUWWuY1WfOecsEGjAKBggqhkjO
PQQDAgNJADBGAiEApGBn8TIm0C+TIEb88eREHkeXeb/XDn6MAusGOgJ/WF4CIQCp
b2KVU4+gZ3X24bzFhmwz2uvSfM7P5Ozas1UhE1+WYQ==
-----END CERTIFICATE-----
PEM

STORE = OpenSSL::X509::Store.new
STORE.add_cert(OpenSSL::X509::Certificate.new(CA_PEM))
KEY = OpenSSL::PKey.read(KEY_PEM)

# One verified connection on loopback to a server presenting `pem`, the
# client asking for `hostname`. A refused handshake can reach the server as
# the client's alert or as the reset of its close, so either reads as
# refused there.
def exchange(pem, hostname)
  server_ctx = OpenSSL::SSL::SSLContext.new
  server_ctx.add_certificate(OpenSSL::X509::Certificate.new(pem), KEY)
  server = TCPServer.new("127.0.0.1", 0)
  port = server.addr[1]
  t = Thread.new do
    sock = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(sock, server_ctx)
    begin
      ssl.accept
      ssl.write("Mangwon: #{ssl.gets}")
      ssl.close
      "accepted"
    rescue OpenSSL::SSL::SSLError, SystemCallError
      "refused"
    ensure
      sock.close
    end
  end

  ctx = OpenSSL::SSL::SSLContext.new
  ctx.verify_mode = OpenSSL::SSL::VERIFY_PEER
  ctx.verify_hostname = true
  ctx.cert_store = STORE
  sock = TCPSocket.new("127.0.0.1", port)
  ssl = OpenSSL::SSL::SSLSocket.new(sock, ctx)
  ssl.hostname = hostname
  client = begin
    ssl.connect
    ssl.write("ferry\n")
    line = ssl.gets
    ssl.close
    line.chomp
  rescue OpenSSL::SSL::SSLError
    "refused"
  end
  sock.close
  p [hostname, client, t.value]
  server.close
end

# No subjectAltName: the CN is the name.
exchange(NO_SAN_PEM, "pier.mangwon.example")
exchange(NO_SAN_PEM, "ferry.mangwon.example")

# A DNS name in subjectAltName: it is the name, and the CN is not.
exchange(DNS_SAN_PEM, "ferry.mangwon.example")
exchange(DNS_SAN_PEM, "pier.mangwon.example")

# An IP address in subjectAltName and no DNS name: the CN is not a name
# either.
exchange(IP_SAN_PEM, "pier.mangwon.example")
