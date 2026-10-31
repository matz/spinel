class Array
  def concat(x)
    yield :override
    self
  end
end
def discard(a)
  a.concat([2]) { |s| p s }
  a
end
def value(a)
  a.concat([3]) { |s| p s }
end
a = [1]
p discard(a).equal?(a)
p value(a).equal?(a)
p a
