# A class method named like a builtin String method (pack, strip, join)
# does not change that builtin on an untyped receiver: its String went
# into a String handle local as the class method's handle would, and the
# C did not build.
module K
  def self.pack(n) = [n].pack("N") + "".b
  def self.strip = "k".b + ""
  def self.join = "j".b + ""
end

class C
  def initialize(groups:) = @groups = groups

  def hello
    g = @groups.pack("n*")
    [g.bytesize].pack("n") + g
  end

  def others(s, a)
    t = s.strip
    u = a.join
    [t.bytesize].pack("n") + t + u
  end

  def direct = [@groups.pack("n*"), @groups.pack("C*").bytesize, "#{@groups.pack("C*")}!"]
end

opts = { groups: [1, 2], name: "a", list: ["b"] }
p C.new(groups: opts[:groups] || [3, 4]).hello
p C.new(groups: [5]).hello
p C.new(groups: opts[:groups] || [3, 4]).direct
p C.new(groups: [1]).others(opts[:name] || " x ", opts[:list] || [1, 2])
p K.pack(7), K.strip, K.join
