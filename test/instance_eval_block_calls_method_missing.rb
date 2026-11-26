# In the literal block of x.instance_eval or x.instance_exec, or a proc a
# local holds that only x.instance_exec(&b) runs, self is x: a bare call
# there, and self.m, are calls on x's classes, so a call on their value
# that method_missing answers reaches the hook.
class Record
  def initialize(attrs)
    @attrs = attrs
  end
  def method_missing(name, *args)
    return @attrs[name] if @attrs.key?(name)
    raise NoMethodError, "undefined method '#{name}'"
  end
  def row
    self
  end
  def other
    Record.new(@attrs)
  end
  def direct
    row.token
  end
end
record = Record.new({ token: "abc" })
p record.direct
p record.instance_eval { row.token }
p record.instance_eval { self.row.token }
p record.instance_eval { other.token }
p record.instance_exec(1) { |n| [row.token, n] }
blk = proc { row.token }
p record.instance_exec(&blk)
lam = lambda { self.row.token }
p record.instance_exec(&lam)
p record.instance_eval { r = row; r.token }
begin
  record.instance_eval { row.nope }
rescue NoMethodError => e
  p e.message
end
