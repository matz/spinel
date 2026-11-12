# `@x ||= nil` defines @x, as `@x = nil` does: instance_variables,
# inspect and instance_variable_defined? see it after the write runs.

class Memo
  def cached
    @cached ||= nil
  end
end

m = Memo.new
p m.instance_variables
p m.instance_variable_defined?(:@cached)
p m.cached
p m.instance_variables
puts m.inspect.sub(/0x\h+/, "X")
p m.instance_variable_defined?(:@cached)
m.instance_variable_set(:@cached, 7)
p m.cached

# the value form answers nil
class Probe
  def empty? = (@seen ||= nil).nil?
end
pr = Probe.new
p pr.empty?, pr.instance_variables

# a Struct body and an instance_eval block
S = Struct.new(:z) do
  def touch = @extra ||= nil
end
s = S.new(1)
s.touch
p s.instance_variables

o = Memo.new
o.instance_eval { @other ||= nil }
o.cached
p o.instance_variables.sort

# a copy keeps it
p m.dup.instance_variables

# &&= on an unset ivar writes nothing
class Gate
  def shut = @open &&= nil
end
g = Gate.new
p g.shut, g.instance_variables

# defined?(@x) after the write runs
class Check
  def ask = defined?(@q)
  def touch = @q ||= nil
end
ch = Check.new
p ch.ask
ch.touch
p ch.ask
