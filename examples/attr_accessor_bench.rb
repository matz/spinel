# attr_accessor benchmark (from yjit-bench)

class TheClass
  attr_accessor :levar

  def initialize
    @v0 = 1
    @v1 = 2
    @v2 = 3
    @levar = 1
  end

  def get_value_loop
    sum = 0
    i = 0
    while i < 1000000
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      sum = sum + levar
      i = i + 1
    end
    sum
  end
end

obj = TheClass.new
total = 0
n = 0
while n < 10
  total = total + obj.get_value_loop
  n = n + 1
end
puts total
puts "done"
