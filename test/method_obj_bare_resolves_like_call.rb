# A bare `method(:m)` (or `self.method(:m)`) names the method a bare `m`
# would call: in a class method the class's own class method, in an
# instance method the instance method, before a top-level def of the name.

def show(s)
  p [:top, s]
end

class K
  def show(s)
    p [:inst, s]
  end

  def self.show(s)
    p [:cls, s]
  end

  def self.run(x)
    yield x
    yield "again"
  end

  def go
    method(:show).call(1)
    [2].each(&method(:show))
  end

  def self.go
    method(:show).call(3)
    [4].each(&method(:show))
    run("five", &method(:show))
    self.method(:show).call(6)
    run("seven", &self.method(:show))
  end
end

class Only
  def self.label(s)
    p s
  end

  def self.each_label
    yield "x"
    yield :y
  end

  def self.go
    method(:label).call("one")
    each_label(&method(:label))
  end
end

K.new.go
K.go
Only.go
method(:show).call(8)
