# A first body that may make an instance (record_version_span_main.rb): the
# lambda makes a Gong, and calling it makes one before Gong's first body
# ends, so the include that follows lands where it can be seen
# (--dump-load-record: its own ring, not idle).
module Chime
  def ring = "chime"
end

class Gong
  def ring = "gong 1"
  MAKER = -> { Gong.new }
  FIRST = MAKER.call
  include Chime
end

puts Gong::FIRST.ring

class Gong
  def ring = "gong 2"
end

puts Gong::FIRST.ring, Gong.new.ring
