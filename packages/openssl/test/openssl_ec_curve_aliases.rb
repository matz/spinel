# spinel: not-cruby
# EC.generate already accepts OBJ_txt2nid aliases here; CRuby accepts only
# short names. Each alias must report the same name as CRuby's canonical key.
require "openssl"

["prime256v1", "1.2.840.10045.3.1.7", "secp384r1", "1.3.132.0.34"].each do |curve|
  puts OpenSSL::PKey::EC.generate(curve).freeze.group.curve_name
end

# The distinct long names in OpenSSL's built-in curve table are binary-field
# curves. A library built without those curves must reject both spellings.
begin
  supported = OpenSSL::PKey::EC.generate("Oakley-EC2N-3").group.curve_name
rescue OpenSSL::PKey::PKeyError
  supported = ""
end
begin
  p OpenSSL::PKey::EC.generate("ipsec3").group.curve_name == supported
rescue OpenSSL::PKey::PKeyError
  p supported.empty?
end
