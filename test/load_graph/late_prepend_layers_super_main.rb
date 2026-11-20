# A super in a layer in front of another whose method a method body defines
# later passes that layer until the body runs, as does the super of a
# subclass's def into a superclass that has such a layer: it runs the method
# behind the layer, statically, where the layer's module defines none yet.

# a placed module over a late one over the class's own def
module Q1; def self.setup = (def j(x) = [:q1, x, super]); end
module Q2; def j(x) = [:q2, x, super]; end
class K1; def j(x) = [:k1, x]; end
class K1; prepend Q1; end
class K1; prepend Q2; end
k1 = K1.new
p [k1.j(1), [k1, nil].map { |o| o&.j(2) }]
Q1.setup
p [k1.j(1), [k1, nil].map { |o| o&.j(2) }]

# the layers behind a body's prepend, one late, and a reopening's, late too
module L1; def self.setup = (def j = [:l1, super]); end
module L2; def self.setup = (def j = [:l2, super]); end
class K2
  def j = [:k2]
  prepend L1
end
class K2; prepend L2; end
p [K2.new.j]
L2.setup
p [K2.new.j]
L1.setup
p [K2.new.j]

# a subclass whose own def and prepend lead into a late layer of its parent
module P3; def j = [:p3, super]; end
module M3; def self.setup = (def j = [:m3, super]); end
class A3; def j = [:a3]; end
class B3 < A3; def j = [:b3, super]; end
class A3; prepend M3; end
class B3; prepend P3; end
p [B3.new.j, A3.new.j]
M3.setup
p [B3.new.j, A3.new.j]

# three classes deep, a late layer and a placed one at each level
module R4; def self.setup = (def j(x) = [:r4, x, super(x + 1)]); end
module P4; def j(x) = [:p4, x, super]; end
class A4; def j(x) = [:a4, x]; end
class B4 < A4; def j(x) = [:b4, x, super]; end
class C4 < B4; def j(x) = [:c4, x, super]; end
class A4; prepend R4; end
class B4; prepend P4; end
class C4; prepend P4; end
p [C4.new.j(1), B4.new.j(1), A4.new.j(1)]
R4.setup
p [C4.new.j(1), B4.new.j(1), A4.new.j(1)]

# the same modules late in two classes, in the opposite order
module T1; def self.setup = (def j = [:t1, super]); end
module T2; def j = [:t2, super]; end
module T3; def self.setup = (def j = [:t3, super]); end
class A5; def j = :a5; end
class B5; def j = :b5; end
class A5; prepend T1; prepend T2; prepend T3; end
class B5; prepend T3; prepend T2; prepend T1; end
class C5 < A5; def j = [:c5, super]; end
p [A5.new.j, B5.new.j, C5.new.j]
T3.setup
p [A5.new.j, B5.new.j, C5.new.j]
T1.setup
p [A5.new.j, B5.new.j, C5.new.j]

# includes between: a class's own def with a late include behind it
module I6; def self.setup = (def j = [:i6, super]); end
module J6; def j = [:j6, super]; end
class B6; def j = [:b6]; end
class K6 < B6; def j = [:k6, super]; end
class K6; include J6; end
class K6; include I6; end
p [K6.new.j]
I6.setup
p [K6.new.j]

# a class's own def over a late include, behind which the superclass's method is guarded by a prepend that did not
# run, and one that did: the super reaches the arms of that guard
module I9; def self.setup = (def jk = [:i9, super]); end
module P9; def jk = [:p9, super]; end
class B9; def jk = [:b9]; end
if ARGV.size > 99
  class B9; prepend P9; end
end
class K9 < B9; include I9; def jk = [:k9, super]; end
p K9.new.jk
I9.setup
p K9.new.jk
module I10; def self.setup = (def jk = [:i10, super]); end
module P10; def jk = [:p10, super]; end
class B10; def jk = [:b10]; end
class K10 < B10; include I10; def jk = [:k10, super]; end
p K10.new.jk
class B10; prepend P10; end
p K10.new.jk
I10.setup
p K10.new.jk

# a layer's body passes its block, or its arguments and block, on to the layer behind it
# (the entry's arm runs the body, its block the entry's own)
module QF; def self.setup = (def j(&b) = [:qf, b&.call, super(&b)]); end
module PF; def self.setup = (def j(&b) = [:pf, b&.call]); end
class KF; end
class KF; prepend PF; end
class KF; prepend QF; end
QF.setup
PF.setup
p KF.new.j { 1 }
module QG; def self.setup = (def j(x, &b) = [:qg, x, b&.call, super(x + 1, &b)]); end
module PG; def self.setup = (def j(x, &b) = [:pg, x, b&.call]); end
class KG; end
class KG < Object; end
class KG; prepend PG; end
class KG; prepend QG; end
QG.setup
PG.setup
p KG.new.j(1) { 2 }
