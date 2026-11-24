# The forms of a class body in a value apply at its statement as a statement's own body's do: attr, alias,
# visibility words, a mixin, an extend, a `class << self`, a def after a raise a rescue modifier caught.
class K
  def m = 0
  def self.cm = 0
end
module M; def mm = :mm; end
module N; def m = [:pre, super]; end
module E; def em = :em; end

z = [(class K; attr_reader :ar; alias m2 m; private; def pv = 9; end; 0)]
k = K.new
p k.respond_to?(:ar), k.respond_to?(:m2), k.respond_to?(:pv)
p((k.pv rescue $!.class))

z = [(class K; prepend N; end; 0)]
p K.new.m
z = [(class K; include M; end; 0)]
p K.new.mm
z = [(class K; extend E; end; 0)]
p K.em
z = [(class K; class << self; def sc = 7; end; end; 0)]
p K.sc

z = [(class K; def after_raise = 2; raise "x" rescue nil; def self.after_cm = 3; end; 0)]
p K.new.after_raise, K.after_cm

begin
  z = [(class K; def part_a = 1; raise "x"; def part_b = 2; end; 0)]
rescue => e
  p e.message
end
p K.new.part_a

z = [(module Mo; def self.a = 1; def b = 2; module_function :b; end; 0)]
p Mo.a, Mo.b

# an alias takes the visibility its original has in the same body
z = [(class K; def y1 = 1; private :y1; alias_method :w1, :y1; end; 0)]
p((K.new.w1 rescue $!.class))
p((K.new.y1 rescue $!.class))

# a mixin earlier in the body, or in a body before it in the statement, has run at a later alias and call
module MS; def source = 1; end
module MT; def tsrc = :t; end
class KS; end
z = [(class KS; include MS; alias copied source; alias_method :copied2, :source; end; 0)]
p KS.new.copied, KS.new.copied2
class KT; end
z = [(class KT; include MS; end; 0), (class KT; alias copied source; prepend MT; alias tcopy tsrc; end; 0)]
p KT.new.copied, KT.new.tcopy
class KN; end
begin
  z = [(class KN; alias copied source; include MS; end; 0)]
rescue NameError => e
  p e.message
end
p((KN.new.copied rescue $!.class), (KN.new.source rescue $!.class))
