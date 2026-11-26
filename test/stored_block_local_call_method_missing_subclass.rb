# A block a class method stores, read into a local and called, in a class
# with method_missing and a subclass: `self.class` answers a Class, which
# method_missing never answers, so the local holds the block alone.
class Record
  def self.audience
    @audience
  end

  def self.allow(&check)
    @audience = check
  end

  def allowed?(row)
    check = self.class.audience
    check.call(row)
  end

  def both?(row)
    check = self.class.audience
    [check.(row), check[row], check.call(row, @user)]
  end

  def method_missing(name, *args)
    raise NoMethodError, "undefined method '#{name}'"
  end
end

class Draft < Record
  allow { |row, *| row == 42 }
end
p Draft.new.allowed?(42)
p Draft.new.allowed?(7)
p Draft.new.both?(42)
