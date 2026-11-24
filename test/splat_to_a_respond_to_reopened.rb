# A splat asks the respond_to? that holds where it runs, with the arguments
# that def takes: the one-parameter def before the reopening that gives it two,
# the two-parameter def before the reopening that gives it one.
class C
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
  def respond_to?(n) = n == :to_a
end

class D < C; end

class E < D; end

class F
  def method_missing(n, *a) = n == :to_a ? [:conv] : super
  def respond_to?(n, priv = false) = n == :to_a
end

x = ARGV.empty? ? C.new : nil
y = ARGV.empty? ? nil : C.new
p [*C.new]
p [*D.new]
p [*E.new]
p [*C.new, *D.new]
p [*x]
p [*y]
p [[*C.new], [*F.new]]
p [1, *F.new, 2]

class C
  def respond_to?(n, priv = false) = n == :to_a
end

class F
  def respond_to?(n) = n == :to_a
end
