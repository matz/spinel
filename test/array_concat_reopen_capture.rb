class Array
  def concat(x)
    yield :override
    self
  end
end
n = 0
a = [1]
a.concat([2]) { |s| p s; n += 1 }
r = a.concat([3]) { |s| p s; n += 1 }
p [n, r.equal?(a), a]
