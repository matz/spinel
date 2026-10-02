# A bare call to a yielding method of the class chain, beside a top-level
# def of the same name that does not yield: the class's method is the
# callee, as Ruby resolves it, and its value is the call's (nil here).

def go
  :top
end

class C
  def go
    yield
    nil
  end

  def self.go
    yield
    nil
  end

  def run
    x = go { puts "instance block" }
    p x
  end

  def self.run
    x = go { puts "class block" }
    p x
  end
end

class Base
  def step
    yield
    nil
  end
end

def step
  :top_step
end

class D < Base
  def run
    x = step { puts "inherited block" }
    p x
  end
end

C.new.run
C.run
D.new.run
p go
p step
