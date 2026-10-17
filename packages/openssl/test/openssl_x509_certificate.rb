# X509::Certificate in and out: new and load over PEM and DER, to_der and
# to_pem, and CertificateError for what is not a certificate.
require "openssl"

# A chain in fullchain.pem order, the leaf and then the CA that issued it:
# a P-256 leaf for turn.jayang-dong.example under "Han River Test CA".
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

certs = OpenSSL::X509::Certificate.load(LEAF_PEM + CA_PEM)
p certs.size
p certs.map(&:class)
p certs.map(&:to_pem) == [LEAF_PEM, CA_PEM]

leaf = OpenSSL::X509::Certificate.new(LEAF_PEM)
der = leaf.to_der
p der == certs[0].to_der
p der.bytesize, der.encoding, leaf.to_pem.encoding
# The subject's name rides in the DER as its own bytes.
p der.include?("turn.jayang-dong.example"), certs[1].to_der.include?("Han River Test CA")

# What to_der answers is the caller's: changing it leaves the certificate as
# it was.
mine = leaf.to_der
mine << "Seoul"
p leaf.to_der.bytesize, leaf.to_pem == LEAF_PEM

# DER in, to either constructor.
p OpenSSL::X509::Certificate.new(der).to_pem == LEAF_PEM
p OpenSSL::X509::Certificate.load(der).map(&:to_der) == [der]

# Text before the first BEGIN line is skipped, as some bundles carry a
# comment there.
p OpenSSL::X509::Certificate.load("# Han River Test CA
" + CA_PEM).size

["Incheon Airport", ""].each do |bad|
  begin
    OpenSSL::X509::Certificate.load(bad)
  rescue OpenSSL::X509::CertificateError => e
    p e.message
  end
end

begin
  OpenSSL::X509::Certificate.new("Gimpo Airport")
rescue OpenSSL::X509::CertificateError => e
  p e.class
end

# A chain whose second certificate does not parse fails whole, rather than
# answering the leaf alone.
begin
  OpenSSL::X509::Certificate.load(LEAF_PEM + CA_PEM.sub("MII", "XXX"))
rescue OpenSSL::X509::CertificateError => e
  p e.class
end

p OpenSSL::X509::CertificateError.superclass
