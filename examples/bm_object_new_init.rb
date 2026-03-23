# object-new-initialize benchmark (from yjit-bench)
class C
  def initialize(a, b, c, d)
    @a = a
    @b = b
    @c = c
    @d = d
    @e = "c"
  end
end

def test
  C.new(1, 2, 3, 4)
end

i = 0
while i < 1000000
  test
  i = i + 1
end
puts "done"
