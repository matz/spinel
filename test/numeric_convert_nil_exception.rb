a = begin
  Rational(nil, exception: false)
rescue => e
  [e.class, e.message]
end
b = begin
  Complex(nil, exception: false)
rescue => e
  [e.class, e.message]
end
p [a, b]

[false, true].each do |flag|
  begin
    p Rational(nil, exception: flag)
  rescue => e
    p [e.class, e.message]
  end
  begin
    p Complex(nil, exception: flag)
  rescue => e
    p [e.class, e.message]
  end
end
begin
  Rational(nil)
rescue => e
  p [e.class, e.message]
end
begin
  Complex(nil)
rescue => e
  p [e.class, e.message]
end
begin
  Rational(nil, exception: nil)
rescue => e
  p [e.class, e.message]
end
begin
  Complex(nil, exception: 0)
rescue => e
  p [e.class, e.message]
end
def conversion_flag
  puts "flag"
  false
end
p Rational((puts "arg"; nil), exception: conversion_flag)
p Complex((puts "arg"; nil), exception: conversion_flag)

values = [nil, 2]
p Rational(values[0], exception: false), Complex(values[0], exception: false)
p Rational(values[1], exception: false), Complex(values[1], exception: false)
begin
  Rational(nil, {exception: false})
rescue => e
  p [e.class, e.message]
end
begin
  Complex(nil, {exception: false})
rescue => e
  p [e.class, e.message]
end
p Rational(0.nonzero?, exception: false), Complex(0.nonzero?, exception: false)
p Rational(2.nonzero?, exception: false), Complex(2.nonzero?, exception: false)
# A local that may hold nil but holds a number converts as before.
i = 3
v = i.even? ? nil : i
p Rational(v, exception: false)
x = 2.5
x = nil if x > 3
p Complex(x, exception: false)
# A Complex read from an Array keeps its imaginary part.
c = [Complex(1, 2), "x"][0]
p Complex(c, exception: false)
