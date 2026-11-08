require "openssl"

begin
  OpenSSL::PKey::PKey.allocate.verify(nil, "", "")
rescue => error
  p error.class
  p error.message == "PKEY wasn't initialized!"
end

begin
  OpenSSL::PKey::PKey.allocate.verify("SHA256", nil, nil)
rescue => error
  p error.class
  p error.message == "PKEY wasn't initialized!"
end
