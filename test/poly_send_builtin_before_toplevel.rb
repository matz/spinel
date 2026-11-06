# A value whose class is known only at run time takes its own builtin
# method for send(:name), ahead of a top-level def of that name (Object's).
def to_s = "main"
def greet = "hi"
class Q; end
class R; def to_s = "r"; end
p [3, "a", :s, 1.5, [1], nil].map { |v| v.send(:to_s) }
p [Q.new, 3].map { |v| v.send(:to_s) }
p [R.new, Q.new, "b"].map { |v| v.send(:to_s) }
p [3, Q.new].map { |v| v.send(:greet) }
x = [3, "a"].sample
p x.send(:to_s).size
p [3, "ab"].map { |v| v.send(:to_s).size }
