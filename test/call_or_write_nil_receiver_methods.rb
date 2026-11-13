# `recv.v ||= x` and `recv.v &&= x` on a nil receiver, where a half of the
# pair is a def (or private, or missing), raise NoMethodError for the reader
# on nil, as CRuby does, before the value or either half runs.
class Both
  def initialize; @v = nil; end
  def v; @v; end
  def v=(x); puts "Both#v="; @v = x; end
end
class ReadAttr
  attr_reader :v
  def initialize; @v = nil; end
  def v=(x); @v = x; end
end
class WriteAttr
  attr_writer :v
  def initialize; @v = 1; end
  def v; @v; end
end
class Priv
  def initialize; @v = nil; end
  def v=(x); @v = x; end
  private
  def v; @v; end
end
class NoReader
  attr_writer :v
end
def pick(k, n) = n.odd? ? k.new : nil
def side = (puts "value ran"; 7)

[Both, ReadAttr, WriteAttr, Priv, NoReader].each_with_index do |k, i|
  o = pick(k, 2)
  begin; o.v ||= side; rescue NoMethodError => e; puts "#{i} or #{e.message}"; end
  begin; o.v &&= side; rescue NoMethodError => e; puts "#{i} and #{e.message}"; end
  begin; x = (o.v ||= side); p x; rescue NoMethodError => e; puts "#{i} or value #{e.message}"; end
end

# one class per receiver
ob = pick(Both, 2)
begin; ob.v ||= side; rescue NoMethodError => e; puts "Both #{e.message}"; end
begin; p(ob.v &&= side); rescue NoMethodError => e; puts "Both value #{e.message}"; end
orr = pick(ReadAttr, 2)
begin; orr.v ||= side; rescue NoMethodError => e; puts "ReadAttr #{e.message}"; end
begin; p(orr.v ||= side); rescue NoMethodError => e; puts "ReadAttr value #{e.message}"; end
ow = pick(WriteAttr, 2)
begin; ow.v &&= side; rescue NoMethodError => e; puts "WriteAttr #{e.message}"; end
op = pick(Priv, 2)
begin; op.v ||= side; rescue NoMethodError => e; puts "Priv #{e.message}"; end
on = pick(NoReader, 2)
begin; on.v ||= side; rescue NoMethodError => e; puts "NoReader #{e.message}"; end
begin; p(on.v ||= side); rescue NoMethodError => e; puts "NoReader value #{e.message}"; end

b = pick(Both, 1); b.v ||= 3; p b.v
r = pick(ReadAttr, 1); p(r.v ||= 4); p r.v
w = pick(WriteAttr, 1); w.v &&= 5; p w.v
begin; pick(Priv, 1).v ||= 1; rescue NoMethodError => e; puts e.message; end
puts "after"
