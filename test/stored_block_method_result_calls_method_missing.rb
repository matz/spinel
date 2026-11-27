# A block a class keeps in a table and runs with instance_exec: a call
# that method_missing answers, on the value of a method the block calls
# with no receiver, reaches the hook. The value is typed as the one class
# its method returns, so the call is split on that class.
class Record
  def self.bodies
    @bodies ||= {}
  end
  def self.body(name, &block)
    bodies[name] = block
  end
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
    Record.new({ token: "other" })
  end
  def run(name)
    self.instance_exec(&self.class.bodies[name])
  end

  body :bare do
    token
  end
  body :through_row do
    row.token
  end
  body :through_self_row do
    self.row.token
  end
  body :through_other do
    other.token
  end
  body :held do
    r = row
    r.token
  end
  body :missing do
    row.nope
  end
end

class Invitation < Record
  body :sub do
    row.token
  end
end

record = Record.new({ token: "abc" })
p record.run(:bare)
p record.run(:through_row)
p record.run(:through_self_row)
p record.run(:through_other)
p record.run(:held)
begin
  record.run(:missing)
rescue NoMethodError => e
  p e.message
end
p Invitation.new({ token: "inv" }).run(:sub)
