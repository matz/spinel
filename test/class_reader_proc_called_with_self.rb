# A Proc a class method hands back through self.class, called with self as
# an argument, in a class with method_missing and a subclass: the call's
# receiver is the Proc, not what it is handed (.class answers Class objects
# only, so a call on its value is no builtin's that hands on its receiver).
class Record
  def self.audience
    @audience
  end
  def self.allow(&check)
    @audience = check
  end
  def allowed?(row)
    check = self.class.audience
    check.call(row, self)
  end
  def method_missing(name, *args)
    raise NoMethodError, "undefined method '#{name}'"
  end
end
class Draft < Record
  allow { |row, rec| row == 42 && rec.is_a?(Draft) }
end
p Draft.new.allowed?(42)
p Draft.new.allowed?(7)
p [1, "a", nil, 2.0].map { |x| x.class }
p Draft.new.class.audience.call(42, Draft.new)
