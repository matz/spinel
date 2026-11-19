# A reopened builtin's own #to_json, as CRuby's json calls it: Integer,
# Float, String, nil, true, false, Array and Hash are written by the
# generator itself, any other value through its #to_json. A reopened
# Integer used to put an arm on a struct that does not exist in the hook
# (the program did not compile once every class was instantiated), and a
# reopened Time, Range or Symbol inside a container serialized its #to_s.
require "json"

class Integer
  def to_json(*) = "int:" + to_s
end

class String
  def to_json(*) = "str:" + self
end

class Time
  def to_json(*) = "time:" + to_i.to_s
end

class Range
  def to_json(*) = "rng"
end

class Symbol
  def to_json(*) = '"sym:' + to_s + '"'
end

class Pt
  def initialize
    @x = 1
  end

  def to_json(*) = '"pt:' + @x.to_s + '"'
end

Marshal.load(Marshal.dump(Pt.new))   # every class instantiated
t = Time.new(2020, 1, 2, 3, 4, 5, "+00:00")
p 7.to_json
p "s".to_json
p({ "n" => 1, "s" => "s", "p" => Pt.new, "t" => t, "r" => (1..2), "y" => :a }.to_json)
p [1, "s", t, :b, (3..4)].to_json
p JSON.generate(:z)
p JSON.generate([:z, t])
puts JSON.pretty_generate({ "y" => :a, "n" => [1, :c] })
