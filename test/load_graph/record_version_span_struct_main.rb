# A body that makes an instance of its class with a builder's `[]`
# (record_version_span_main.rb): `Point[0]` makes a Point before the
# reopening's body ends, so the include that follows lands where it can be
# seen (--dump-load-record: its own ring, not idle). Lone's first body makes a Point,
# which is no Lone, so its include lands before any Lone exists.
Point = Struct.new(:x) do
  def twice = x * 2
end

module Chime
  def ring = "chime"
end

class Point
  def ring = "point 1"
  puts Point[0].ring
  include Chime
end

class Lone
  def ring = "lone 1"
  ORIGIN = Point[1]
  include Chime
end

puts Point.new(2).ring, Lone.new.ring

class Point
  def ring = "point 2"
end

class Lone
  def ring = "lone 2"
end

puts Point.new(3).ring, Lone.new.ring, Lone::ORIGIN.twice
