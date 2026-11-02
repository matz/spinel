# An undef, by the keyword or by undef_method with literal names, takes the
# method away at its point: a definition of the name after it in the same
# class gives it back, and respond_to? between the two answers false. The
# undef was applied to the whole class, so the later def raised
# NoMethodError.

class P
  def a = 1
  def b = 2
  def c = 3
end

class Q < P
  p new.respond_to?(:a)
  undef_method :a, "b"
  p new.respond_to?(:a), new.respond_to?(:b)
  self.undef_method :c
  def a = 10
end
p Q.new.a, Q.new.respond_to?(:c)
begin
  Q.new.b
rescue NoMethodError => e
  puts e.message
end
p P.new.b

class R < P
  p new.respond_to?(:a)
  undef a
  def a = 20
end
p R.new.a
