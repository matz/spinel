# spinel: not-cruby -- CRuby returns a Time with a Bignum second count past int64; spinel raises RangeError
# spinel: int64
def try(label)
  puts "#{label}: #{yield}"
rescue RangeError => e
  puts "#{label}: RangeError #{e.message}"
end
try("round") { Time.at(9223372036854775807, 999999999, :nsec).round.to_i }
try("ceil") { Time.at(9223372036854775807, 1, :nsec).ceil.to_i }
try("round in range") { Time.at(9223372036854775806, 999999999, :nsec).round.to_i }
try("usec 1e30") { Time.at(0, 1e30, :usec).to_i }
try("usec -1e30") { Time.at(0, -1e30).to_i }
try("nsec carry") { Time.at(9223372036854775807, 10**9, :nsec).to_i }
try("nsec borrow") { Time.at(-9223372036854775808, -1, :nsec).to_i }
try("usec carry") { Time.at(9223372036854775807, 10**18).to_i }
try("in range") { Time.at(9223372036854775806, 10**9, :nsec).to_i }
