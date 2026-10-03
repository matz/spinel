# Typed slices and windows bind boxed parameters, including missing elements.
RowBox = Struct.new(:v)
def integer_rows(a, n)
  c = RowBox.new(a).v
  c.each_slice(2) { |q, qr| p [q, qr] }
  c.each_cons(2) { |q, qr| p [q, qr] }
  c.each_slice(n) { |q, qr| p [q, qr] }
  c.each_cons(n) { |q, qr| p [q, qr] }
  c[0] = 9
end
integer_rows([3, 1, 2], 1)
FloatRowBox = Struct.new(:v)
def float_rows(a)
  c = FloatRowBox.new(a).v
  c.each_slice(2) { |q, qr| p [q, qr] }
  c.each_cons(2) { |q, qr| p [q, qr] }
  c[0] = 9.5
end
float_rows([3.5, 1.5, 2.5])
