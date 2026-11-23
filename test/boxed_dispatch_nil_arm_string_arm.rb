# frozen_string_literal: true
# A call on a boxed receiver whose classes answer through different arms: one
# answers nil, another answers a String it keeps in an ivar, a third answers
# an Integer, and an alias names the String arm. The caller mutates what the
# call answered and prints what the object holds.

class Quiet
  def pick = nil
end

class Keeper
  def initialize = @s = +"kept"
  def pick = @s
  alias_method :grab, :pick
  def held = @s
end

class Count
  def pick = 7
end

class Fresh
  def pick = +"fresh"
end

objs = [Quiet.new, Keeper.new, Count.new, Fresh.new]
keeper = objs[1]

objs.each do |o|
  v = o.pick
  v << "!" if v.is_a?(String)
  p v
end
p keeper.held

objs.each do |o|
  v = o.respond_to?(:grab) ? o.grab : o.pick
  v << "?" if v.is_a?(String)
  p v
end
p keeper.held

# the answer of a nil arm and a String arm in one expression
objs.each do |o|
  w = o.pick || +"dflt"
  w << "#" if w.is_a?(String)
  puts w
end
p keeper.held

# through a local that is later reassigned
v = objs[1].pick
objs[0].pick
v << "~"
p keeper.held, v.equal?(keeper.held)
