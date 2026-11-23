# A method made by a declaration sent by a literal name in a class body:
# send, __send__ and public_send of attr_*, alias_method and define_method,
# on self with no receiver, with self., and on the class by its constant;
# beside a sent include and a sent private, which the load record reads
# itself; in an included hook on its parameter; and beside a send of the
# program's own, which keeps its call.
module SdCmp
  def same?(o) = o.class == self.class
end
class SdThing
  send(:include, SdCmp)
  self.send(:attr_reader, :a)
  SdThing.__send__(:attr_writer, :a)
  def initialize = @a = 1
  def hidden = :h
  def also = :also
  send(:private, :hidden)
  send(:alias_method, :other, :also)
  public_send(:define_method, :twice) { a * 2 }
  def peek = hidden
end
t = SdThing.new
t.a = 4
p t.a, t.twice, t.other, t.peek, t.same?(SdThing.new), t.respond_to?(:hidden)
begin
  t.hidden
rescue NoMethodError => e
  puts "private: #{e.class}"
end
module SdMixer
  def self.included(base)
    base.send(:attr_accessor, :mixed)
    base.__send__(:private, :secret)
  end
end
class SdHost
  def secret = 1
  include SdMixer
  def reveal = secret
end
h = SdHost.new
h.mixed = 9
p h.mixed, h.reveal, h.respond_to?(:secret)

class SdRecorder
  def self.send(name, *args)
    puts "own send #{name} #{args.inspect}"
  end
  send(:attr_accessor, :v)
  __send__(:attr_reader, :r)
  def initialize = @r = 2
end
p SdRecorder.new.r, SdRecorder.new.respond_to?(:v)

module SdSender
  def self.included(base)
    base.send(:define_method, :twice) { 6 }
    base.send(:include, Comparable)
  end
end
class SdA
  include SdSender
  def <=>(o) = 0
end
p SdA.new.twice, SdA.new == SdA.new
