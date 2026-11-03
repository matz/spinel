# A private method of a reopened Object, when a reopened builtin (String)
# has a public method of the same name: a String answers its own, and an
# object of a program class that has none raises NoMethodError, also when
# it is boxed or reached by public_send.
class Object
  private def top = "t"
end
class String; def top = "str"; end
class D; def top = "own"; end
class I; end

def try
  yield
rescue NoMethodError => e
  "NME #{e.message}"
end

[D.new, I.new, "x"].each do |x|
  p [try { x.top }, try { x.public_send(:top) }, x.send(:top)]
end
p [try { I.new.top }, try { I.new.public_send(:top) }, "y".public_send(:top)]
