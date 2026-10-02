# A block given to a bare call takes its parameters' types from the method
# the call reaches: the class's own method before a top-level def, and in a
# class method the class method before an instance method of the name.

def each_item
  yield 1
  yield 2
end

class Box
  def each_item
    yield "a"
    yield "b"
  end

  def show
    each_item { |x| p x }
  end
end

class Crate
  def each_pair
    yield 1, 2
  end

  def self.each_pair
    yield "k", :v
  end

  def self.show
    each_pair { |k, v| p [k, v] }
  end
end

Box.new.show
Crate.show
each_item { |x| p x }
Crate.new.each_pair { |a, b| p a + b }
