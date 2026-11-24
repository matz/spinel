# spinel: not-cruby -- CRuby returns a Time with a Bignum second count past int64; spinel raises RangeError
# spinel: int64
[9.2e18, -9.2e18, -9223372036854775808.0, 1e12, 9223372036854775808.0, 9.3e18, 1e19, 1e300, -9.3e18, -1e300].each do |f|
  begin
    puts "#{f}: #{Time.at(f).to_i}"
  rescue RangeError => e
    puts "#{f}: RangeError #{e.message}"
  end
end
t = Time.at(9.2e18)
[1e17, 1e300, -1e300].each do |f|
  begin
    puts "+ #{f}: #{(t + f).to_i}"
  rescue RangeError => e
    puts "+ #{f}: RangeError #{e.message}"
  end
end
begin
  puts "- #{1e300}: #{(t - 1e300).to_i}"
rescue RangeError => e
  puts "- #{1e300}: RangeError #{e.message}"
end
max = Time.at(9223372036854775807)
min = Time.at(-9223372036854775808)
[0.5, 1.0].each do |f|
  begin
    puts "max + #{f}: #{(max + f).to_i}"
  rescue RangeError => e
    puts "max + #{f}: RangeError #{e.message}"
  end
end
[[max, 1], [t, 100000000000000000], [min, -1]].each do |base, i|
  begin
    puts "+ #{i}: #{(base + i).to_i}"
  rescue RangeError => e
    puts "+ #{i}: RangeError #{e.message}"
  end
end
[[min, 1], [max, -1], [max, 0]].each do |base, i|
  begin
    puts "- #{i}: #{(base - i).to_i}"
  rescue RangeError => e
    puts "- #{i}: RangeError #{e.message}"
  end
end
