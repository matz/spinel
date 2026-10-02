# A class method hands `&method(:m)` to a yielding class method by bare
# name: each yield calls m, which types m's parameters, even beside an
# instance method of the same name.

def show(s)
  p s
end

def twice(n)
  p n * 2
end

class K
  def run(x)
    :inst
  end

  def self.run(x)
    yield x
    yield "two"
  end

  def self.each_num
    yield 3
    yield 4
  end

  def self.go
    run("one", &method(:show))
    each_num(&method(:twice))
  end
end

K.go
p K.new.run(1)
