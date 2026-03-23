# getivar-module benchmark (from yjit-bench)
# Uses module-level ivar access via class method

module TheModule
  @levar = 1

  def self.get_value_loop
    sum = 0
    i = 0
    while i < 1000000
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      sum = sum + @levar
      i = i + 1
    end
    sum
  end
end

total = 0
idx = 0
while idx < 10
  total = total + TheModule.get_value_loop
  idx = idx + 1
end
puts total
puts "done"
