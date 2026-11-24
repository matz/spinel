# spinel: not-cruby -- CRuby returns a Time with a Bignum second count past int64; spinel raises RangeError
# spinel: int64
def try(label)
  puts "#{label}: #{yield}"
rescue RangeError => e
  puts "#{label}: RangeError #{e.message}"
end
try("2**64") { Time.at(2**64).to_i }
try("-2**64") { Time.at(-(2**64)).to_i }
try("2**64 usec") { Time.at(2**64, 5).to_i }
try("2**64 nsec") { Time.at(2**64, 5, :nsec).to_i }
try("2**64 in") { Time.at(2**64, in: "UTC").to_i }
try("2**70 - 2**70 + 7") { Time.at(2**70 - 2**70 + 7).to_i }
