# Building a certificate as CRuby does -- Certificate.new, its setters and
# #sign -- and reading its fields back, from a certificate built here and
# from one loaded from PEM. A self-signed P-256 certificate made this way
# serves a TLS handshake. ECDSA signatures are random, so what is printed is
# what does not depend on them.
require "openssl"
require "socket"

# X509::Name: from an Array, from a parse, from its DER, and entry by entry
name = OpenSSL::X509::Name.new([["CN", "héllo"], ["C", "KR"], ["emailAddress", "a@example.org"],
                                ["O", "Example", OpenSSL::ASN1::PRINTABLESTRING]])
p name.to_a
p name.to_s
p OpenSSL::X509::Name.new(name.to_der).to_s
p OpenSSL::X509::Name.new(name.to_der) == name
p OpenSSL::X509::Name.parse("/CN=media/O=Example").to_a
p OpenSSL::X509::Name.parse_openssl("CN=media, O=Example").to_a
p OpenSSL::X509::Name.parse("/CN=media/O=Example") == OpenSSL::X509::Name.new([["CN", "media"], ["O", "Example"]])
p OpenSSL::X509::Name.new([["CN", "Media"]]) == OpenSSL::X509::Name.new([["CN", "media"]])
p OpenSSL::X509::Name.new([["CN", "a"]]) == OpenSSL::X509::Name.new([["CN", "b"]])
p OpenSSL::X509::Name.new([["CN", "a"]]) == "a"
p OpenSSL::X509::Name.new([["CN", "a"]]) <=> OpenSSL::X509::Name.new([["CN", "b"]])
p OpenSSL::X509::Name.new([["CN", "b"]]).cmp(OpenSSL::X509::Name.new([["CN", "a"]]))
empty = OpenSSL::X509::Name.new
p [empty.to_s, empty.to_a, empty.to_der.unpack1("H*")]
p empty.add_entry("CN", "z").equal?(empty)
p empty.add_entry("O", "y", OpenSSL::ASN1::IA5STRING).to_a
begin
  empty.add_entry("NOPE", "x")
rescue OpenSSL::X509::NameError => e
  puts "#{e.class}: #{e.message}"
end
p OpenSSL::X509::Name::OBJECT_TYPE_TEMPLATE["C"], OpenSSL::X509::Name::OBJECT_TYPE_TEMPLATE["CN"]
p [OpenSSL::ASN1::UTF8STRING, OpenSSL::ASN1::PRINTABLESTRING, OpenSSL::ASN1::IA5STRING]

# A new certificate, before it has anything
cert = OpenSSL::X509::Certificate.new
p [cert.version, cert.serial.to_i, cert.subject.to_s, cert.issuer.to_a]
begin
  cert.to_der
rescue OpenSSL::X509::CertificateError => e
  puts "#{e.class}: #{e.message}"
end

# Self-signed, as a DTLS-SRTP endpoint makes its own at start-up
key = OpenSSL::PKey::EC.generate("prime256v1")
subject = OpenSSL::X509::Name.parse("/CN=media.example/O=Example")
cert.version = 2
cert.serial = 0x1234_5678
cert.subject = subject
cert.issuer = subject
cert.not_before = Time.at(1_700_000_000)
cert.not_after = Time.at(1_700_000_000 + 30 * 86400)
cert.public_key = key
p [cert.version, cert.serial.to_s, cert.subject.to_s, cert.not_before, cert.not_after]
begin
  cert.to_der
rescue OpenSSL::X509::CertificateError => e
  puts "unsigned: #{e.class}"
end
p cert.sign(key, "SHA256").equal?(cert)
p cert.verify(key)
p cert.verify(OpenSSL::PKey::EC.generate("prime256v1"))
p [cert.version, cert.serial.to_s, cert.serial.class, cert.subject.to_s, cert.issuer.to_s]
p [cert.not_before, cert.not_before.utc?, cert.not_after]
p cert.public_key.class
p cert.public_key.public_to_der == key.public_to_der

# Round trips: DER and PEM give the same certificate back, readers included
der = cert.to_der
again = OpenSSL::X509::Certificate.new(der)
p again.to_der == der
p OpenSSL::X509::Certificate.new(cert.to_pem).to_der == der
p [again.version, again.serial.to_s, again.subject == subject, again.not_after.to_i]
p again.verify(key)
p OpenSSL::X509::Certificate.load(cert.to_pem + cert.to_pem).map { |c| c.to_der == der }

# A change after signing keeps the old signature until the certificate is
# signed again (whether verify sees the change before that depends on the
# OpenSSL version CRuby runs on, so it is not read here)
again.subject = OpenSSL::X509::Name.parse("/CN=other.example")
p again.subject.to_s
again.sign(key, OpenSSL::Digest.new("SHA384"))
p [again.subject.to_s, again.verify(key)]

# An RSA key signs one too, read from PEM (the key openssl_server_rsa.rb uses)
RSA_PEM = <<~PEM
-----BEGIN PRIVATE KEY-----
MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCuKHGt1duTXstD
NEBfMwIZY6Ee333kgKrahcOHqpGOIIOxxarIBBInD1YZR+38R9oNu0tbiUtlpvOp
0CO8ozKktsIieb667zrcGVyI//KTgtx8xaHamcu/jfwjbWNYbxtz7Vx2PM4NO3OE
9cAD1cj9Jx20biy6VQyJcBYLjkNdIJYXWJx5Ij2oatwvFJN06WrXzhGeefw57yfW
ivju3qBatdy8YB0nvZ17QBq7yEorWXCOH7xcOCoLAFu8oKGVP2//dAWjuzRn6PAh
jJ59RoOpo1j6bjKYJtUDPw+heq7QF175Xv7sKczLo8Syn2xrGl6LC3/TyHJ6gzhX
QzWo9GH9AgMBAAECggEAF/5gcwekobAMpuya9cKsRl0whHVb/dAwObz2DEnWldMq
8aH+/mF5MsJtJTyAsCIiC4RDRJAswAt5RGr1kXaWcpTvOBq3yT9OR6aC1ikU/Xeo
8tOzkc9gf7qLA0J9zmA4UwdflraX2MDZEHdfBeg9MYtjD0miheVTvdegYeUuqLSr
Qq7oso4vr6dsQsg8+mVWfeofnbDie8AUjiY7NeGAEp7jA61fDgVulcbX/hi42q7V
qODfFpjs6fBxGa6zbDMhWz6dqDa7PTH5ne4A6TqWfdg0cvZfkD6H/0uop3csmIfH
TfFh+EFyDVvMli030TTNwe0JT/SwzKaBhbbRbpqWDQKBgQDjMTlFdj8iRN73G1yI
mfHYJBJ9pcCaWoVN8P8zGebefpaJjWNo+NToTxO+D9wUjp5tI3l9DeUwhtUAJJIs
V9pEq8MzIM4ROmcXGfbJNlKd00reYtUCJrSCzyt8Ivj5tEaf1sHfIn+fyRRiHeC9
0DPLwdZfaO3Ox65UlLeRrjEkpwKBgQDEPbNGhmRcokKpRkljGbtaspTGpNAsWjka
gb/KXX8Atxgpdc1cnicYfMwcHQC9Cx4uxs9qusY3KOQFMtdzM4M8cQl083of3M7q
skDVx+qw48sopaNhpYdLqKVP+2DmtOITNcLO0lQ6mMAnaKlN0u/nDxqNk7X1HnOF
Mjf3aSQEuwKBgFf8h0acfeTisUsYAKUyoZPonHSmzcm5hujhjaQXwu6mLdzFshtE
SwhRH7862yvm8/omg/VnfTxm63TrtOSRfYmcz7MsYEIR0T0EJCxW6nYPD27C7eWw
YbZLT3b7AFuMctGSVXD6Jl0Wfdtl4NUCHh0qS8CVnYu4j45YLiOwR33/AoGAOAAb
fs6vKTgsqb2jB6K6MmGvzxgNR/rfPM81+fvAWGXngT1aw0lmgrhqTsOACHJ5xIN7
gUuXcPo8R7eAgCF9G+xt9G/es+UBLzx7AAay1JPMz+akvyLLfy27Lx3fQzbvJOg8
lWS0IJyiiRx24/yD9X3759b1H+P2t1XjXrkkTC0CgYBBtnkV+zDGR4SoublalEPx
irE3XvK2IkyDMKFUaCGshVgZ0h4co7wKL3Nyh1i/8PHn914ak77Gh9gMgcLitNgz
kKJEgovRpvFnTvc6ivVxdkMjA0a0C+6xlCDuPML7yy32hONbXgMrUdFcm/49lgIy
M3S+PteQRbYKXCT1Vl48eQ==
-----END PRIVATE KEY-----
PEM
rsa = OpenSSL::PKey.read(RSA_PEM)
rc = OpenSSL::X509::Certificate.new
rc.version = 2
rc.serial = OpenSSL::BN.new("7")
rc.subject = rc.issuer = OpenSSL::X509::Name.parse("/CN=rsa.example")
rc.not_before = Time.at(1_700_000_000)
rc.not_after = Time.at(1_800_000_000)
rc.public_key = rsa
rc.sign(rsa, "SHA256")
p [rc.verify(rsa), rc.public_key.class, rc.serial.to_i]
begin
  rc.verify(key)
rescue OpenSSL::X509::CertificateError => e
  puts "#{e.class}: #{e.message}"
end
p OpenSSL::X509::Certificate.new(rc.to_der).to_der == rc.to_der
p again.public_key.private?

# A certificate loaded from PEM reads the same way
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
leaf = OpenSSL::X509::Certificate.new(LEAF_PEM)
p [leaf.version, leaf.serial.to_i, leaf.subject.to_s, leaf.issuer.to_s]
p [leaf.not_before, leaf.not_after]
p leaf.subject.to_a
p leaf.public_key.class

# The self-signed certificate serves a handshake; the client does not
# verify it, as a DTLS-SRTP peer compares the fingerprint instead
ctx = OpenSSL::SSL::SSLContext.new
ctx.cert = cert
ctx.key = key
server = OpenSSL::SSL::SSLServer.new(TCPServer.new("127.0.0.1", 0), ctx)
port = server.addr[1]
t = Thread.new do
  ssl = server.accept
  ssl.write("hello #{ssl.gets}")
  ssl.close
  "served"
end
cctx = OpenSSL::SSL::SSLContext.new
cctx.verify_mode = OpenSSL::SSL::VERIFY_NONE
sock = TCPSocket.new("127.0.0.1", port)
ssl = OpenSSL::SSL::SSLSocket.new(sock, cctx)
ssl.connect
ssl.write("from the client\n")
p ssl.gets
ssl.close
sock.close
p t.value
server.close
