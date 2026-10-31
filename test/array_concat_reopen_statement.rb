class Array
  def concat(x)
    puts :override
    self
  end
end
a = [1]
a.concat([2])
p a
r = a.concat([3])
p r.equal?(a)
p a
