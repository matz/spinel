# x.to_i, x.to_f and Integer(x) of an Integer that can be nil never answer
# nil (nil.to_i is 0, Integer(nil) raises): the fields written from them hold
# no nil bit. Integer(x, exception: false) hands x's nil on, where it read x
# as a plain Integer and raised TypeError.
class K
  def initialize(x)
    @a = x.to_i
    @b = (Integer(x) rescue 7)
    @c = Integer(x, exception: false) || 5
    @d = x.to_f
  end
  attr_reader :a, :b, :c, :d
end
x = [4][ARGV.size + 1]
k = K.new(x)
p [k.a, k.b, k.c, k.d]
k = K.new(3)
p [k.a, k.b, k.c, k.d]
x = [4][ARGV.size + 1]
y = Integer(x, exception: false)
p y, y.nil?
w = [4][ARGV.size]
z = Integer(w, exception: false)
p z
