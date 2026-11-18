# spinel: not-cruby -- Unavailable decoding forms raise NotImplementedError rather than implying malformed input.
# CRuby supplies these forms; this subset also bounds native recursion at 1000 levels.
require "openssl"

def unsupported(der)
  begin
    begin
      OpenSSL::ASN1.decode(der)
      puts "unexpected success"
    rescue OpenSSL::ASN1::ASN1Error
      puts "unexpected ASN1Error"
    rescue => error
      puts "unexpected StandardError"
    end
  rescue NotImplementedError => error
    puts error.class, error.message
  end
end

["30800201010000", "1f020101", "2203020101", "1003020101", "040161", "0500",
 "a003020101", "1f1f0101", "3100", "0101ff", "4000", "c000", "0000"].each do |hex|
  unsupported([hex].pack("H*"))
end

[1001, 3000].each do |depth|
  der = "\x02\x01\x07".b
  depth.times { der = OpenSSL::ASN1::Sequence.new([der]).to_der }
  unsupported(der)
end
