require "openssl"

class ECDSABytes
  def initialize(bytes) = @bytes = bytes
  def to_str = @bytes
end

["prime256v1", "secp384r1"].each do |curve|
  key = OpenSSL::PKey::EC.generate(curve).freeze
  public_key = OpenSSL::PKey.read(key.public_to_der).freeze
  digest = OpenSSL::Digest.digest(curve == "prime256v1" ? "SHA256" : "SHA384", "message\0bytes")
  signature = key.dsa_sign_asn1(digest)
  puts signature.class
  p public_key.dsa_verify_asn1(digest, signature)
  p key.dsa_verify_asn1(digest, signature)
  p public_key.dsa_verify_asn1(ECDSABytes.new(digest), ECDSABytes.new(signature))
  p public_key.dsa_verify_asn1(digest, key.dsa_sign_asn1(ECDSABytes.new(digest)))
  p public_key.dsa_verify_asn1("\0" * digest.bytesize, signature)
  p public_key.dsa_verify_asn1(digest, "\x30\x06\x02\x01\x00\x02\x01\x00")
  ["broken", "", signature + "\0"].each do |invalid|
    begin
      public_key.dsa_verify_asn1(digest, invalid)
    rescue OpenSSL::PKey::PKeyError => error
      puts error.class
    end
  end
  begin
    public_key.dsa_sign_asn1(digest)
  rescue OpenSSL::PKey::PKeyError => error
    puts error.class
  end
  ["", "\0", "d" * 100].each do |hash|
    p public_key.dsa_verify_asn1(hash, key.dsa_sign_asn1(hash))
  end
  [nil, Object.new].each do |invalid|
    begin
      key.dsa_sign_asn1(invalid)
    rescue TypeError => error
      puts error.class
    end
    begin
      public_key.dsa_sign_asn1(invalid)
    rescue TypeError => error
      puts error.class
    end
    begin
      public_key.dsa_verify_asn1(digest, invalid)
    rescue TypeError => error
      puts error.class
    end
  end
end

# RFC 6979 A.2.5/A.2.6, "sample" signatures: these pin verification to an
# already-computed digest independently of the signing implementation.
[
  ["SHA256",
   "3059301306072a8648ce3d020106082a8648ce3d03010703420004" \
   "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6" \
   "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299",
   "3046022100EFD48B2AACB6A8FD1140DD9CD45E81D69D2C877B56AAF991C34D0EA84EAF3716" \
   "022100F7CB1C942D657C41D436C7A1B6E29F65F3E900DBB9AFF4064DC4AB2F843ACDA8"],
  ["SHA384",
   "3076301006072a8648ce3d020106052b8104002203620004" \
   "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64DEF8F0EA9055866064A254515480BC13" \
   "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1288B231C3AE0D4FE7344FD2533264720",
   "306602310094EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C81A648152E44ACF96E36DD1E80FABE46" \
   "02310099EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94FA329C145786E679E7B82C71A38628AC8"]
].each do |hash, public_hex, signature_hex|
  public_key = OpenSSL::PKey.read([public_hex].pack("H*"))
  signature = [signature_hex].pack("H*")
  digest = OpenSSL::Digest.digest(hash, "sample")
  p public_key.dsa_verify_asn1(digest, signature)
  p public_key.dsa_verify_asn1(OpenSSL::Digest.digest(hash, digest), signature)
end
