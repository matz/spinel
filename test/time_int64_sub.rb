# spinel: int64
p Time.at(9223372036854775807) - Time.at(-9223372036854775808)
p Time.at(-9223372036854775808) - Time.at(9223372036854775807, 500000000, :nsec)
p (Time.at(1700000000, 250000000, :nsec) - Time.at(1600000000)).round(3)
p Time.at(0, 10**17).to_i
p Time.at(0, 10**17, :nsec).to_i
p Time.at(0, 2e19, :nsec).to_i
p Time.at(0, 2e19, :nsec).nsec
p Time.at(0, 9e18 + 1024.0, :nsec).nsec
p Time.at(0, 2.0**63 + 2048.0, :nsec).to_i
p Time.at(0, 2.0**63 + 2048.0, :nsec).nsec
p Time.at(0, -(2.0**63) - 2048.0, :nsec).to_i
p Time.at(0, -(2.0**63) - 2048.0, :nsec).nsec
p Time.at(0, 2.0**70, :nsec).to_i
p Time.at(0, 2.0**70, :nsec).nsec
p Time.at(0, -(2.0**70), :nsec).to_i
p Time.at(0, -(2.0**70), :nsec).nsec
p Time.at(0, 2.0**70).nsec
p Time.at(-9223372036854775808, 2.0**93, :nsec).to_i
p Time.at(-9223372036854775808, 2.0**93, :nsec).nsec
p Time.at(9223372036854775807, -(2.0**93), :nsec).to_i
p Time.at(9223372036854775807, -(2.0**93), :nsec).nsec
begin
  Time.at(0, Float::NAN)
rescue FloatDomainError => e
  puts "FloatDomainError #{e.message}"
end
begin
  Time.at(0, Float::INFINITY, :usec)
rescue FloatDomainError => e
  puts "FloatDomainError #{e.message}"
end
