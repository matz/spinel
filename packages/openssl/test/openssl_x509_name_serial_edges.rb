# X509::Name and Certificate#serial at their edges, as CRuby answers them:
# a template that lacks a field gives UTF8STRING, not add_entry's template;
# an empty String is no name's DER; an OID longer than 255 characters comes
# back from to_a whole; and a serial of any size goes through sign, to_der
# and a certificate read back from it.
require "openssl"

def types(name) = name.to_a.map { |e| e[2] }

p types(OpenSSL::X509::Name.new([["C", "KR"], ["CN", "Mapo"]]))
p types(OpenSSL::X509::Name.new([["C", "KR"], ["CN", "Mapo"]], {}))
p types(OpenSSL::X509::Name.new([["C", "KR"], ["CN", "Mapo"]], { "CN" => OpenSSL::ASN1::IA5STRING }))
p types(OpenSSL::X509::Name.new([["C", "KR", OpenSSL::ASN1::IA5STRING]], {}))
p types(OpenSSL::X509::Name.parse("/C=KR/CN=Mapo", {}))

begin
  OpenSSL::X509::Name.new("")
  p :no_error
rescue OpenSSL::X509::NameError => e
  p e.class
end
p OpenSSL::X509::Name.new("\x30\x00".b).to_a

oid = "1.2" + ".12345" * 45
entry = OpenSSL::X509::Name.new([[oid, "Yeouido"]]).to_a[0]
p entry[0] == oid
p entry[0].size
p entry[1]

KEY = OpenSSL::PKey::EC.generate("prime256v1")

# Whether the serial survives: on the certificate that was signed, and on
# one read back from its DER.
def round_trip(serial)
  c = OpenSSL::X509::Certificate.new
  c.version = 2
  c.serial = serial
  c.subject = c.issuer = OpenSSL::X509::Name.parse("/CN=turn.jayang-dong.example")
  c.not_before = Time.at(0)
  c.not_after = Time.at(86400)
  c.public_key = KEY
  c.sign(KEY, "SHA256")
  [c.serial == serial, OpenSSL::X509::Certificate.new(c.to_der).serial == serial]
end

p round_trip(2**63)
p round_trip(2**159)
p round_trip(2**255)
p round_trip(-(2**200))
p round_trip(10**520)
p OpenSSL::X509::Certificate.new.tap { |c| c.serial = 10**520 }.serial.to_s.size
