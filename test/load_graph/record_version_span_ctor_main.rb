# The load record's spans (--dump-load-record,
# record_version_span_ctor_main.rb.load-record) for objects made by a
# constructor named by a Symbol: each class's first ring runs where its
# object is made, so the version before its redefinition is seen
# (the include stands behind the class's own ring), never idle. Bell's object is
# made only by a class method's own constructor, which the first `puts`
# runs; Drum has no object until the last line.
# record_version_span_exception_main.rb makes one by exception, by
# method(:new) and by a builder's [].
module Chime
  def ring = "chime"
end

class Sent
  def ring = "sent 1"
  FIRST = Sent.send(:new).ring
  include Chime
end

class Public
  def ring = "public 1"
  FIRST = Public.public_send(:new).ring
  include Chime
end

class Bare
  def ring = "bare 1"
  FIRST = Bare.__send__(:allocate).ring
  include Chime
end


class Bell
  def ring = "bell 1"
  def self.make = new
end

class Drum
  def ring = "drum 1"
end

puts Bell.make.class

class Bell
  include Chime
end

class Drum
  include Chime
end

class Sent
  def ring = "sent 2"
end

class Public
  def ring = "public 2"
end

class Bare
  def ring = "bare 2"
end

class Bell
  def ring = "bell 2"
end

class Drum
  def ring = "drum 2"
end

puts Sent::FIRST, Public::FIRST, Bare::FIRST
puts Sent.new.ring, Bell.new.ring, Drum.new.ring
