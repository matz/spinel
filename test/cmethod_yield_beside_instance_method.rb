# Inside a class method self is the class: a bare call to a yielding class
# method answers that method's value, not that of an instance method of
# the same name.

class C
  def go
    :inst
  end

  def self.go
    yield
    nil
  end

  def self.run
    x = go { puts "class block" }
    p x
  end
end

class D
  def twice
    "instance"
  end

  def self.twice
    a = yield
    b = yield
    a + b
  end

  def self.run
    n = 0
    v = twice { n += 1 }
    p v, n
  end
end

C.run
p C.new.go
D.run
p D.new.twice
