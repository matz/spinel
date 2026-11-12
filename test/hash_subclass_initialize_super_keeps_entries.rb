class M < Hash
  def initialize(src = nil, default = nil, &blk)
    src.each { |k, v| self[k] = v } if src
    default ? super(default) : super(&blk)
  end
end
p M.new({"a" => 1})
x = M.new({"b" => 2}, "dflt")
p x, x[:zz]
y = M.new({"c" => 3}) { |h, k| "d:#{k}" }
p y, y[:q]
class N < Hash
  def initialize(src)
    self["x"] = src
    self.default = 4
    super()
  end
end
n = N.new(1)
p n, n[:none]
