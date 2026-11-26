# A call on a Method value that holds nil (super_method of a method with
# no super) raises NoMethodError for the call's name on nil.
class C
  def hi(x) = x
end

m = C.new.method(:hi).super_method
p m
[-> { m.call(2) }, -> { m.(2) }, -> { m[2] }].each do |f|
  p f.call
rescue NoMethodError => e
  p [e.class, e.message]
end
p C.new.method(:hi).call(3)
