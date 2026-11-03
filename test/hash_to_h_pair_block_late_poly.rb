# Hash#to_h with a block on a receiver typed only late: an ivar that
# starts as {} and is filled from a subclass body. The block's
# pairs build the Hash and its parameters hold the entry's key and value.
class Attr
  attr_reader :default
  def initialize(default) = @default = default
  def check(value) = value.is_a?(String) ? "ok #{value}" : "bad"
end

class Kind
  attr_reader :fields
  def initialize = @fields = {}
end

class Rec
  def self.kind = @kind ||= Kind.new
  def self.complete(values)
    kind.fields.to_h do |name, attr|
      value = values.key?(name) ? values[name] : attr.default
      puts attr.check(value)
      [name, value]
    end
  end
  def self.names = kind.fields.to_h { |name| [name, true] }
end

class Todo < Rec
  kind.fields[:text] = Attr.new("")
  kind.fields[:note] = Attr.new("n")
end

p Todo.complete({ text: "hi" })
p Todo.complete({})
p Todo.names

# A boxed value that is a Hash or an Array of pairs: a block of one
# parameter binds a Hash's key and an Array's element.
[{ a: 1 }, [[:b, 2]]].each do |v|
  p(v.to_h { |k, x| [k, x] })
  p(v.to_h { |e| [e, 0] })
end

# A Struct in a boxed slot yields its [member, value] pairs.
Pt = Struct.new(:x, :y)
[Pt.new(1, 2), { z: 3 }].each do |v|
  p(v.to_h { |k, x| [k.to_s, x] })
  p(v.to_h { |k| [k, 1] })
end
