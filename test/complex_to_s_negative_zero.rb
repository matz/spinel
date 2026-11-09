# A Complex whose imaginary part is a Float -0.0 writes "-0.0i" in to_s,
# inspect and an interpolation, as CRuby does: the sign is the Float's sign
# bit. It was written "+-0.0*i", because -0.0 is not less than zero, so the
# sign came out "+" and the magnitude kept its own "-" (which then took the
# "*" a non-numeric magnitude gets). The real part, a NaN or an Infinity
# imaginary part, and an Integer imaginary part were right and stay so; a
# NaN is written "+NaN*i" whatever its sign bit.
inf = Float::INFINITY
nan = Float::NAN
cases = [
  ["(1, -0.0)", Complex(1, -0.0)],
  ["(0, -0.0)", Complex(0, -0.0)],
  ["(-0.0, -0.0)", Complex(-0.0, -0.0)],
  ["(0.0, -0.0)", Complex(0.0, -0.0)],
  ["(1.5, -0.0)", Complex(1.5, -0.0)],
  ["(-0.0, 0.0)", Complex(-0.0, 0.0)],
  ["(-0.0, 1)", Complex(-0.0, 1)],
  ["(1, 0.0)", Complex(1, 0.0)],
  ["(1, -inf)", Complex(1, -inf)],
  ["(1, inf)", Complex(1, inf)],
  ["(-inf, -inf)", Complex(-inf, -inf)],
  ["(1, nan)", Complex(1, nan)],
  ["(1, -nan)", Complex(1, -nan)],
  ["(nan, nan)", Complex(nan, nan)],
  ["(1, -2.5)", Complex(1, -2.5)],
  ["(-1, -2)", Complex(-1, -2)],
  ["(1, 2)", Complex(1, 2)],
  ["(1.0, 2.0)", Complex(1.0, 2.0)],
]
cases.each do |label, z|
  puts "#{label}: to_s=#{z.to_s.inspect} inspect=#{z.inspect} interp=#{"#{z}"}"
end

# the values an operation makes with a -0.0 part
puts Complex(1, 0.0).conjugate.to_s
puts (-Complex(1, 0.0)).to_s
puts (-Complex(1, 0)).to_s
p Complex(1, 0.0).conjugate
p [Complex(1, -0.0)]
puts [Complex(0, -0.0), Complex(2, 3)].join(" ")
