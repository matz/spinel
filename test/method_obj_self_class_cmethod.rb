# `self.class.method(:m)` in an instance method names the class's class
# method, called directly, handed as a block, or turned into a proc.

class K
  def self.show(s)
    p s
  end

  def self.run(x)
    yield x
  end

  def go
    self.class.method(:show).call("three")
    ["a", "b"].each(&self.class.method(:show))
    pr = self.class.method(:show).to_proc
    pr.call("c")
    self.class.run("d", &self.class.method(:show))
  end
end

K.new.go
K.method(:show).to_proc.call("e")
["f"].each(&K.method(:show))
