# An #each reopened on Object reaches an instance of any user class, and a
# boxed receiver's each walks what it yields. A class with its own #each
# still takes its own.
class Object
  def each
    [self].each { |x| yield x }
  end
end

class Thing
end

class Sub < Thing
end

class Pair
  def each
    yield 10
    yield 20
  end
end

module M
  def self.sum_of(list)
    n = 0
    list.each { |x| n += yield x }
    n
  end
end

def run(list) = M.sum_of(list) { |x| x.is_a?(Integer) ? x : 1 }
puts run([1, 2])
puts run(Thing.new)
puts run(Sub.new)
puts run(Pair.new)
