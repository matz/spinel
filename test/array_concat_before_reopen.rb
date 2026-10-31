a = [1]
a.concat([2])
p a
class Array
  def concat(x)
    puts :override
    self
  end
end
a.concat([3])
p a.concat([4]).equal?(a)
p a
