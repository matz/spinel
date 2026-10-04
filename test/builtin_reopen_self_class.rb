# `self.class` in a method of a reopened Hash, Array or String is that
# builtin -- no program class may derive from it -- so `self.class.new`
# builds one: activesupport's Hash#extract! collects into
# `self.class.new`. It was resolved to the reopening as a user class of its
# own, whose constructor and struct do not exist, and the C did not compile.
# A Numeric reopening serves Integer and Float alike, and a program's own
# Text::String is a class of its own: both keep the run-time answer.
class Hash
  def extract!(*keys)
    keys.each_with_object(self.class.new) { |key, result| result[key] = delete(key) if has_key?(key) }
  end
end

class Array
  def emptied = self.class.new
  def kind_name = self.class.name
end

class String
  def blank_copy = self.class.new
end

class Numeric
  def kind_name = self.class.name
end

module Text
  class String
    def initialize(v = "") = @v = v
    def kind_name = self.class.name
  end
  class Rich < String; end
end

h = { a: 1, b: 2, c: 3 }
p h.extract!(:a, :b), h
p [1, 2].emptied, [1].kind_name
p "abc".blank_copy
p 1.kind_name, 1.5.kind_name
p Text::String.new.kind_name, Text::Rich.new.kind_name
