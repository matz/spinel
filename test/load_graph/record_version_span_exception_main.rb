# The load record's spans (--dump-load-record,
# record_version_span_exception_main.rb.load-record) for objects made by
# exception, called directly or by raise, by method(:new) and by a
# builder's []: each class's first ring runs where its object is made,
# so the version before its redefinition is seen (the include
# stands behind the class's own ring), never idle; Bell has no object until the last
# line. The record only: the compiler does not compile these
# constructors right yet, so the program has no .expected.
module Chime
  def ring = "chime"
end

Point = Struct.new(:x) do
  def twice = x * 2
end

class Fault < StandardError
  def ring = "fault 1"
  FIRST = Fault.exception("x").ring
  include Chime
end

class Raised < StandardError
  def ring = "raised 1"
  FIRST = begin
    raise Raised.exception("x")
  rescue Raised => e
    e.ring
  end
  include Chime
end

class Called
  def ring = "called 1"
  FIRST = Called.method(:new).call.ring
  include Chime
end

class Point
  def ring = "point 1"
end

POINT = Point.send(:[], 0).ring

class Point
  include Chime
end

class Bell < StandardError
  def ring = "bell 1"
  include Chime
end

class Fault
  def ring = "fault 2"
end

class Raised
  def ring = "raised 2"
end

class Called
  def ring = "called 2"
end

class Point
  def ring = "point 2"
end

class Bell
  def ring = "bell 2"
end

puts Fault::FIRST, Raised::FIRST, Called::FIRST, POINT, Bell.new.ring
