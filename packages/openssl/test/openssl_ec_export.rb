require "openssl"

["prime256v1", "secp384r1"].each do |curve|
  key = OpenSSL::PKey::EC.generate(curve).freeze
  puts key.group.class
  puts key.group.curve_name
  group = key.group.freeze
  p group.curve_name.encoding == Encoding::BINARY
  name = group.curve_name
  name.replace("changed")
  p group.curve_name == curve
  p OpenSSL::PKey::EC::Group.new(group).curve_name == curve
  public_der = key.public_to_der
  # id-ecPublicKey belongs to SPKI/PKCS#8, rather than SEC1 ECPrivateKey.
  p public_der.include?("\x06\x07\x2a\x86\x48\xce\x3d\x02\x01".b)
  puts public_der.bytesize
  public_key = OpenSSL::PKey.read(public_der).freeze
  puts public_key.class
  p public_key.public?
  p public_key.private?
  p public_key.public_to_der == public_der
  [key.private_to_der, key.private_to_pem].each do |encoded|
    copy = OpenSSL::PKey.read(encoded)
    p copy.private?
    p copy.group.curve_name == curve
    p copy.public_to_der == public_der
    p OpenSSL::PKey.read(copy.private_to_der).public_to_der == public_der
  end
  p key.private_to_der.include?("\x06\x07\x2a\x86\x48\xce\x3d\x02\x01".b)
  p key.private_to_pem.start_with?("-----BEGIN PRIVATE KEY-----\n")
  begin
    public_key.private_to_der
  rescue OpenSSL::PKey::PKeyError => error
    puts error.class
  end
  begin
    public_key.private_to_pem
  rescue OpenSSL::PKey::PKeyError => error
    puts error.class
  end
end

["bogus", "1.2.840.10045.3.1.7", "prime256v1\0suffix"].each do |curve|
  begin
    OpenSSL::PKey::EC::Group.new(curve)
  rescue OpenSSL::OpenSSLError, ArgumentError => error
    puts error.class
  end
end

# RFC 6979 A.2.5's public point, in compressed and hybrid SPKI encodings.
x = "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
y = "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299"
compressed = ["3039301306072a8648ce3d020106082a8648ce3d03010703220003" + x].pack("H*")
hybrid = ["3059301306072a8648ce3d020106082a8648ce3d03010703420007" + x + y].pack("H*")
[compressed, hybrid].each do |encoded|
  p OpenSSL::PKey.read(encoded).public_to_der == encoded
end
# The same published test key as SEC1, retaining its compressed public point.
scalar = "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721"
sec1 = ["30570201010420" + scalar + "a00a06082a8648ce3d030107a12403220003" + x].pack("H*")
key = OpenSSL::PKey.read(sec1)
p key.public_to_der == compressed
[key.private_to_der, key.private_to_pem].each do |encoded|
  p OpenSSL::PKey.read(encoded).public_to_der == compressed
end
# An imported SEC1 key can omit its optional public-key field.
sec1 = ["30310201010420" + scalar + "a00a06082a8648ce3d030107"].pack("H*")
pkcs8 = ["3041020100301306072a8648ce3d020106082a8648ce3d030107042730250201010420" + scalar].pack("H*")
key = OpenSSL::PKey.read(sec1)
p key.private_to_der == pkcs8
p key.private_to_pem.lines.reject { |line| line.start_with?("-----") }.join.unpack1("m") == pkcs8
