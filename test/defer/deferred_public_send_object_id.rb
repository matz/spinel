# Compiled with --defer-refusals: a BasicObject subclass aliasing object_id
# to __id__ types every poly receiver's `object_id` as a box. A
# `public_send(key)` on a poly self then lists an object_id arm among its
# methods, whose builtin id is boxed for the slot the send answers into.
# The one refusal left is the ruby2_keywords one further down.
# spinel: defer-refusals: 1:10 "b" done
class Proxy < BasicObject
  def initialize(target)
    @target = target
  end
  alias_method :object_id, :__id__
end
# (named, so the program keeps it: a class nothing names is left out)
PROXY = Proxy

class Object
  def with(**attributes)
    old_values = {}
    begin
      attributes.each do |key, value|
        old_values[key] = public_send(key)
        public_send("#{key}=", value)
      end
      yield self
    ensure
      old_values.each do |key, old_value|
        public_send("#{key}=", old_value)
      end
    end
  end
end

class Point
  attr_accessor :x, :y
  def initialize; @x = 1; @y = 2; end
end

class Label
  attr_accessor :text
  def initialize; @text = "a"; end
end

Point.new.with(x: 10) { |o| p o.x }
Label.new.with(text: "b") { |o| p o.text }
puts "done"
proc { "a" }.ruby2_keywords
puts "not reached"
