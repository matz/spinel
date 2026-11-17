require "openssl"

der = ["300d02010706032a030403030000ff"].pack("H*")
workers = 3.times.map do
  Thread.new do
    correct = true
    12.times do
      first = OpenSSL::ASN1.decode(der)
      payload = OpenSSL::ASN1::BitString.new("x" * 300).to_der
      second = OpenSSL::ASN1.decode(payload)
      oid = OpenSSL::ASN1.decode(["06082a8648ce3d030107"].pack("H*"))
      correct &&= first.to_der == der && second.to_der == payload && oid.value == "prime256v1"
    end
    correct
  end
end
p workers.map(&:value)

def copied_decode(input)
  copy = input
  OpenSSL::ASN1.decode(copy)
end
node = copied_decode(der)
p node.to_der == der
p node.value.map(&:tag)

deep = "\x02\x01\x05".b
100.times { deep = OpenSSL::ASN1::Sequence.new([deep]).to_der }
value = OpenSSL::ASN1.decode(deep)
p value.to_der == deep
100.times { value = value.value[0] }
puts value.value.to_i
