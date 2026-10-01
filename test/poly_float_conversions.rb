# A boxed value converted to a Float follows CRuby. Kernel#Complex given a
# boxed Complex builds a + b*i: the component went through sp_poly_to_f,
# which answered 0.0 for any kind it did not name, so Complex(x, 1) with x a
# boxed Complex(1, 2) was (0+1i). An explicit #to_f on a boxed receiver is
# each class's own method: nil.to_f is 0.0, a String parses its leading
# number with underscores, a Complex with an exact-zero imaginary part is
# its real part, and true, a Symbol and an Array have no #to_f. A boxed
# value is a member of a Float range by its value when it is a real number,
# and never when it is not: "2" and :a read as 2.0 and as a Symbol's id.

x = [Complex(1, 2), 1][0]
p Complex(x, 1)
p Complex(1, x)
p Complex(x, x)
p Complex(x)
f = [1.5, 2][0]
p Complex(f, 1)
p Complex(2, f)
s = ["1+2i", 1][0]
p Complex(s, 1)
n = [nil, 1][0]
p((Complex(n, 1) rescue $!))
a = [[1], 1][0]
p((Complex(a, 1) rescue $!))
i = [4, 1.5][0]
p Complex(i, 1), Complex(i)
z = [Complex(3, 0), 1][0]
p Complex(z, 2)

vals = [nil, "1_0.5x", " 2.5", 3, 2**64, 1.5, Rational(1, 4), Complex(2, 0), Time.at(1, 500000)]
vals.each { |v| p v.to_f }
[:a, true, [1], Complex(1, 2)].each do |v|
  begin
    p v.to_f
  rescue NoMethodError, RangeError => e
    puts "#{e.class}: #{e.message[0, 40]}"
  end
end

r = 1.0..3.0
xs = [2, 2.5, "2", :a, nil, 2**64, Rational(3, 2), Complex(2, 0)]
xs.each { |x| print((r === x).inspect, " ") }
puts
xs.each { |x| print(r.cover?(x).inspect, " ") }
puts
xs.each do |x|
  case x
  when 1.0..3.0 then print "in "
  else print "out "
  end
end
puts
