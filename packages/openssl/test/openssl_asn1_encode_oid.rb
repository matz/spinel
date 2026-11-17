require "openssl"

["id-ecPublicKey", "prime256v1", "SHA256", "sha256", "2.16.840.1.101.3.4.2.1",
 "1.2.840.10045.2.1", "1.2.3.4", "2.999.123456789012345678901234567890"].each do |value|
  node = OpenSSL::ASN1::ObjectId.new(value).freeze
  p [node.value, node.tag, node.tag_class]
  p [node.oid, node.sn, node.ln, node.to_der.unpack1("H*")]
  p node.value.equal?(value)
end

# Short and long names are nil for unknown numeric OIDs and invalid names.
["synthetic-unknown-object-id", "1.40", ""].each do |value|
  node = OpenSSL::ASN1::ObjectId.new(value)
  p [node.value, node.sn, node.ln]
end

class SyntheticOidName
  private
  def to_str = "sha256"
end
name = SyntheticOidName.new
node = OpenSSL::ASN1::ObjectId.new(name)
p node.value.equal?(name)
p [node.oid, node.sn, node.ln]
puts node.to_der.unpack1("H*")

class SyntheticObjectId < OpenSSL::ASN1::ObjectId
  def value = "overridden"
end
puts SyntheticObjectId.new("SHA256").value
puts SyntheticObjectId.new("SHA256").oid
puts SyntheticObjectId.new("SHA256").send(:to_der).unpack1("H*")
