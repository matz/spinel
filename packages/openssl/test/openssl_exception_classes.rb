require "openssl"

[OpenSSL::Cipher::CipherError, OpenSSL::Cipher::AuthTagError, OpenSSL::HMACError,
 OpenSSL::PKey::PKeyError, OpenSSL::PKey::ECError].each do |error|
  p error.ancestors.map(&:name)
end
p OpenSSL::PKey::ECError.equal?(OpenSSL::PKey::PKeyError)
p OpenSSL::PKey::ECError < OpenSSL::PKey::PKeyError

begin
  raise OpenSSL::HMACError, "public test failure"
rescue OpenSSL::OpenSSLError => error
  puts error.class
end

begin
  OpenSSL::PKey.read("malformed")
rescue OpenSSL::PKey::ECError => error
  puts error.class
end

begin
  OpenSSL::PKey::EC.generate("not_a_curve")
rescue OpenSSL::PKey::PKeyError => error
  puts error.class
end
