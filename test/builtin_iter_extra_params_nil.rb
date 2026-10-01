# A block's required parameter past the values a builtin step yields is nil
# on every step, also when the slot is typed by something else: an Array
# read back from an instance variable and later written past its end typed
# it an Integer, and it read 0.

def t
  @c = [3, 1]
  c = @c
  c.each { |q, r| p r }
  c.filter! { |*qs| p qs; true }
  c.each_index { |i, j| p [i, j] }
  c.each_with_index { |x, i, z| p [x, i, z] }
  c[5] = 9
  p c
end
t

def u
  a = [3, 1]
  a.each { |q, r| r = q if q > 2; p r }
  a[3] = 7
  2.times { |i, j| p [i, j] }
  1.upto(2) { |i, j| p [i, j] }
  1.step(3, 2) { |i, j| p [i, j] }
  (1..2).each { |i, j| p [i, j] }
  "ab".each_char { |c, d| p [c, d] }
  "ab".each_byte { p [_1, _2] }
  5.tap { |q, r| p [q, r] }
  { a: 1 }.each { |k, v, z| p [k, v, z] }
  p(a.inject(0) { |s, x, z| z.nil? ? s + x.to_i : -1 })
end
u
