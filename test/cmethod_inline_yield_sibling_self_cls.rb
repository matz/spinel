# A yielding class method is inlined at its call sites, where no receiving
# class parameter is declared. A sibling class method it calls through
# implicit self still receives the class the call was made on.
class Base
  def self.helper(x)
    x + 1
  end

  def self.label
    name
  end

  def self.make(x)
    yield x if block_given?
    helper(x)
  end

  def self.tag
    yield label if block_given?
    label
  end
end

class Sub < Base
  def self.make(x)
    x * 10
  end
end

class Other < Base
end

p Base.make(1)
p Base.make(2) { |v| p v }
p Sub.make(3)
p Other.make(4)
p Base.tag
p Other.tag
p(Other.tag { |s| p s })
