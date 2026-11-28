# Certificate#serial= takes what CRuby's takes: an Integer, a Bignum among
# them however the program holds it (here walked out of an Array, so the
# value is boxed), or a BN. Anything else -- a Float, a String, an object
# answering #to_int, nil -- is CRuby's TypeError, not a converted number.
require "openssl"

KEY = OpenSSL::PKey::EC.generate("prime256v1")

def signed(serial)
  c = OpenSSL::X509::Certificate.new
  c.version = 2
  c.serial = serial
  c.subject = c.issuer = OpenSSL::X509::Name.parse("/CN=serial.example")
  c.not_before = Time.at(0)
  c.not_after = Time.at(86400)
  c.public_key = KEY
  c.sign(KEY, "SHA256")
  OpenSSL::X509::Certificate.new(c.to_der).serial
end

[1, 2**63, 2**159, 2**255, -(2**70)].each do |serial|
  p signed(serial) == serial
end
p signed(OpenSSL::BN.new("12345678901234567890123")).to_s

class Seventeen
  def to_int = 17
end

[1.5, "5", Seventeen.new, nil].each do |bad|
  c = OpenSSL::X509::Certificate.new
  begin
    c.serial = bad
    p [:no_error, c.serial.to_i]
  rescue TypeError => e
    p [bad.class, e.message]
  end
end
