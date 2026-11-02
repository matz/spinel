# JSON.parse reads a Float of up to 15 significant digits scaled by at most
# 10^22 with one exact division or product; every other Float goes to the
# general parse. Both give the correctly rounded value: the cases sit on each
# side of the two limits, and a run of generated decimals is checked by its
# bits, in 16-bit pieces so every value stays below 2^31.
require "json"

cases = %w[
  0.0 -0.0 0.5 0.1 0.2 0.3 -2.5 1.0E2 1.5e+3 2.5E-3 -1.25e21 0e5 -0e5 -0.000e-30
  123456789012345.0 1234567890123456.0 12345678901234567.0
  0.123456789012345 0.1234567890123456 0.12345678901234567
  9007199254740993.0 9007199254740992.0 4.35 8.41 1.15 2.675
  1e22 1e23 1.0e22 1.0e-22 1e-22 1e-23 123456789012345e-22 123456789012345e-23
  0.000000000000000000000123 0.00000000001234 1.500000000000000000 1e005 1e0005
  5e-324 2.2250738585072014e-308 1.7976931348623157e308
]
cases.each { |s| puts "#{s} #{JSON.parse("[#{s}]")[0].inspect}" }

p JSON.parse('{"x": 0.50742313, "y": -12.75e-1, "z": [1.5, 2E1, -0.0]}')

seed = 11
h = 0
3000.times do |i|
  seed = (seed * 75 + 74) % 65537
  int = seed.to_s
  seed = (seed * 75 + 74) % 65537
  a = seed % 10000
  seed = (seed * 75 + 74) % 65537
  frac = (a.to_s.rjust(4, "0") + (seed % 100000).to_s.rjust(5, "0"))[0, 1 + i % 9]
  s = "#{i.odd? ? "-" : ""}#{int}.#{frac}"
  s += "e#{i % 7 - 3}" if i % 3 == 0
  f = JSON.parse("[#{s}]")[0]
  [f].pack("G").unpack("nnnn").each { |b| h = (h * 31 + b) % 1000003 }
end
p h
