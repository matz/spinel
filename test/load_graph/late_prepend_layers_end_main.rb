# Where no method is behind the late layers (the class and its superclasses
# define none), a call before their bodies run finds no method: CRuby calls
# the receiver's method_missing, or raises NoMethodError. A layer a body
# has defined takes the call from then on.

# no method_missing: a NoMethodError, for each arity the call asks
module Q1; def self.setup = (def j(x = 1) = [:q1, x]); end
module Q2; def self.setup = (def j(x = 1) = [:q2, x]); end
class K1; end
class K1; prepend Q1; end
class K1; prepend Q2; end
k1 = K1.new
p [(k1.j rescue $!.class), (k1.j(3) rescue $!.class), (k1.public_send(:j) rescue $!.class)]
Q1.setup
p [k1.j, k1.j(3)]
Q2.setup
p [k1.j, k1.j(3)]

# a method_missing of the class answers the call before, and a layer after
module Q3; def self.setup = (def j = [:q3]); end
module Q4; def self.setup = (def j = [:q4]); end
class K2
  def method_missing(n, *a) = n == :j ? [:mm, a] : super
end
class K2; prepend Q3; end
class K2; prepend Q4; end
k2 = K2.new
p [k2.j, [k2, nil].map { |o| o&.j }]
Q3.setup
p [k2.j]
Q4.setup
p [k2.j]

# one layer in front of the class's body, the class having no method; a
# subclass shares it
module Q5; def self.setup = (def j = [:q5]); end
class K3
  prepend Q5
end
class S3 < K3; end
p [(K3.new.j rescue $!.class), (S3.new.j rescue $!.class)]
Q5.setup
p [K3.new.j, S3.new.j]

# an include with a superclass that has none either
module Q6; def self.setup = (def j = [:q6, (super rescue :none)]); end
class P4; end
class K4 < P4; end
class K4; include Q6; end
k4 = K4.new
p [(k4.j rescue $!.class)]
Q6.setup
p [(k4.j rescue $!.class)]

# the class side
module Q7; def self.setup = (def j = [:q7]); end
class K5; end
class << K5; prepend Q7; end
p [(K5.j rescue $!.class)]
Q7.setup
p [K5.j]

# the class's method_missing answers a receiver of the class, and a subclass's
# own one a receiver of the subclass, before and after
module QM1; def self.setup = (def j = [:q6]); end
class KM1; def method_missing(n, *a) = n == :j ? [:kmm] : super; end
class KM1; prepend QM1; end
class SM1 < KM1; def method_missing(n, *a) = n == :j ? [:smm] : super; end
p [KM1.new.j, SM1.new.j]
QM1.setup
p [KM1.new.j, SM1.new.j]
module QM2; def self.setup = (def j = [:q7b]); end
class KM2; end
class KM2; prepend QM2; end
class SM2 < KM2; def method_missing(n, *a) = n == :j ? [:smm] : super; end
class TM2 < SM2; end
class UM2 < KM2; end
p [(KM2.new.j rescue $!.class), SM2.new.j, TM2.new.j, (UM2.new.j rescue $!.class)]
QM2.setup
p [KM2.new.j, SM2.new.j, TM2.new.j, UM2.new.j]

# a block call reaches a layer whose method a body defines later, through a
# class with no method_missing and a subclass with one that takes a block
# (once the layer's body has run, the subclass's receivers run the layer's
# method); and a subclass's method_missing a method body defines is no
# answer of the class's end arm before that body
module QB; def self.setup = (def j(&b) = [:qb, b&.call]); end
class KB; end
class KB; prepend QB; end
class SB < KB; def method_missing(n, *a, &b) = n == :j ? [:smm, b&.call] : super; end
QB.setup
p [SB.new.j { 2 }, KB.new.j { 3 }]
module QC; def self.setup = (def j = [:qc]); end
class KC; end
class KC; prepend QC; end
class SC < KC; def self.arm = (def method_missing(n, *a) = n == :j ? [:smm] : super); end
SC.arm
QC.setup
p [SC.new.j, KC.new.j]

# two late layers in front of a class whose subclass defines method_missing in a
# body, or whose call hands a block: a receiver of the subclass runs the front
# layer's method, whose super runs the layer behind it (once both bodies have run)
module QD; def self.setup = (def j = [:qd, super]); end
module PD; def self.setup = (def j = [:pd]); end
class KD; end
class KD; prepend PD; end
class KD; prepend QD; end
class SD < KD; def self.arm = (def method_missing(n, *a) = n == :j ? [:smm] : super); end
SD.arm
QD.setup
PD.setup
p [SD.new.j, KD.new.j]
module QE; def self.setup = (def j(&b) = [:qe, b&.call, super]); end
module PE; def self.setup = (def j(&b) = [:pe, b&.call]); end
class KE; end
class KE; prepend PE; end
class KE; prepend QE; end
class SE < KE; def method_missing(n, *a, &b) = n == :j ? [:smm, b&.call] : super; end
QE.setup
PE.setup
p [SE.new.j { 4 }, KE.new.j { 5 }]

# a late layer in front of a class whose base gets a later prepend or include (a guard behind the layers),
# with a subclass and a sub-subclass defining method_missing in a method body, called with and without a block:
# the lookup finds the guard's method once it is mixed in
# shape pre 1
module PPRE1; def j(&b) = [:p, b&.call]; end
module Q1PRE1; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
class BasePRE1; end
class KPRE1 < BasePRE1; end
class KPRE1; prepend Q1PRE1; end
class BasePRE1; prepend PPRE1; end
class SPRE1 < KPRE1; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2PRE1 < SPRE1; end
def tPRE1 = yield rescue [$!.class]
def showPRE1 = p([tPRE1 { SPRE1.new.j }, tPRE1 { SPRE1.new.j { 1 } }, tPRE1 { S2PRE1.new.j }, tPRE1 { S2PRE1.new.j { 2 } }, tPRE1 { KPRE1.new.j }, tPRE1 { KPRE1.new.j { 3 } }])
showPRE1
SPRE1.arm
showPRE1
Q1PRE1.setup
showPRE1
# shape pre 2
module PPRE2; def j(&b) = [:p, b&.call]; end
module Q1PRE2; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
module Q2PRE2; def self.setup = (def j(&b) = [:q2, b&.call, super]); end
class BasePRE2; end
class KPRE2 < BasePRE2; end
class KPRE2; prepend Q1PRE2; end
class KPRE2; prepend Q2PRE2; end
class BasePRE2; prepend PPRE2; end
class SPRE2 < KPRE2; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2PRE2 < SPRE2; end
def tPRE2 = yield rescue [$!.class]
def showPRE2 = p([tPRE2 { SPRE2.new.j }, tPRE2 { SPRE2.new.j { 1 } }, tPRE2 { S2PRE2.new.j }, tPRE2 { S2PRE2.new.j { 2 } }, tPRE2 { KPRE2.new.j }, tPRE2 { KPRE2.new.j { 3 } }])
showPRE2
SPRE2.arm
showPRE2
Q2PRE2.setup
showPRE2
Q1PRE2.setup
showPRE2
# shape pre 3
module PPRE3; def j(&b) = [:p, b&.call]; end
module Q1PRE3; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
module Q2PRE3; def self.setup = (def j(&b) = [:q2, b&.call, super]); end
module Q3PRE3; def self.setup = (def j(&b) = [:q3, b&.call, super]); end
class BasePRE3; end
class KPRE3 < BasePRE3; end
class KPRE3; prepend Q1PRE3; end
class KPRE3; prepend Q2PRE3; end
class KPRE3; prepend Q3PRE3; end
class BasePRE3; prepend PPRE3; end
class SPRE3 < KPRE3; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2PRE3 < SPRE3; end
def tPRE3 = yield rescue [$!.class]
def showPRE3 = p([tPRE3 { SPRE3.new.j }, tPRE3 { SPRE3.new.j { 1 } }, tPRE3 { S2PRE3.new.j }, tPRE3 { S2PRE3.new.j { 2 } }, tPRE3 { KPRE3.new.j }, tPRE3 { KPRE3.new.j { 3 } }])
showPRE3
SPRE3.arm
showPRE3
Q3PRE3.setup
showPRE3
Q2PRE3.setup
showPRE3
Q1PRE3.setup
showPRE3
# shape inc 1
module PINC1; def j(&b) = [:p, b&.call]; end
module Q1INC1; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
class BaseINC1; end
class KINC1 < BaseINC1; end
class KINC1; prepend Q1INC1; end
class BaseINC1; include PINC1; end
class SINC1 < KINC1; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2INC1 < SINC1; end
def tINC1 = yield rescue [$!.class]
def showINC1 = p([tINC1 { SINC1.new.j }, tINC1 { SINC1.new.j { 1 } }, tINC1 { S2INC1.new.j }, tINC1 { S2INC1.new.j { 2 } }, tINC1 { KINC1.new.j }, tINC1 { KINC1.new.j { 3 } }])
showINC1
SINC1.arm
showINC1
Q1INC1.setup
showINC1
# shape inc 2
module PINC2; def j(&b) = [:p, b&.call]; end
module Q1INC2; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
module Q2INC2; def self.setup = (def j(&b) = [:q2, b&.call, super]); end
class BaseINC2; end
class KINC2 < BaseINC2; end
class KINC2; prepend Q1INC2; end
class KINC2; prepend Q2INC2; end
class BaseINC2; include PINC2; end
class SINC2 < KINC2; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2INC2 < SINC2; end
def tINC2 = yield rescue [$!.class]
def showINC2 = p([tINC2 { SINC2.new.j }, tINC2 { SINC2.new.j { 1 } }, tINC2 { S2INC2.new.j }, tINC2 { S2INC2.new.j { 2 } }, tINC2 { KINC2.new.j }, tINC2 { KINC2.new.j { 3 } }])
showINC2
SINC2.arm
showINC2
Q2INC2.setup
showINC2
Q1INC2.setup
showINC2
# shape inc 3
module PINC3; def j(&b) = [:p, b&.call]; end
module Q1INC3; def self.setup = (def j(&b) = [:q1, b&.call, super]); end
module Q2INC3; def self.setup = (def j(&b) = [:q2, b&.call, super]); end
module Q3INC3; def self.setup = (def j(&b) = [:q3, b&.call, super]); end
class BaseINC3; end
class KINC3 < BaseINC3; end
class KINC3; prepend Q1INC3; end
class KINC3; prepend Q2INC3; end
class KINC3; prepend Q3INC3; end
class BaseINC3; include PINC3; end
class SINC3 < KINC3; def self.arm = (def method_missing(m, *a, &b) = m == :j ? [:smm, b&.call] : super); end
class S2INC3 < SINC3; end
def tINC3 = yield rescue [$!.class]
def showINC3 = p([tINC3 { SINC3.new.j }, tINC3 { SINC3.new.j { 1 } }, tINC3 { S2INC3.new.j }, tINC3 { S2INC3.new.j { 2 } }, tINC3 { KINC3.new.j }, tINC3 { KINC3.new.j { 3 } }])
showINC3
SINC3.arm
showINC3
Q3INC3.setup
showINC3
Q2INC3.setup
showINC3
Q1INC3.setup
showINC3

# three late layers in front of a class whose subclass defines method_missing in a body: once the bodies have run,
# each layer's super runs the layer behind it
module L1H; def self.setup = (def j(x) = [:l1, x, super]); end
module L2H; def self.setup = (def j(x) = [:l2, x, super]); end
module L3H; def self.setup = (def j(x) = [:l3, x]); end
class KH; end
class KH; prepend L3H; end
class KH; prepend L2H; end
class KH; prepend L1H; end
class SH < KH; def self.arm = (def method_missing(n, *a) = n == :j ? [:smm, a] : super); end
class WH < SH; end
SH.arm
L2H.setup
L3H.setup
L1H.setup
p [SH.new.j(4), WH.new.j(4), KH.new.j(4)]

# a subclass's method_missing that takes a block parameter is no answer before the layer's body has run: its
# receivers run the layer's method after it
module L1I; def self.setup = (def j(x) = [:l1, x]); end
class KI; end
class KI; prepend L1I; end
class SI < KI; def method_missing(m, *a, &b) = m == :j ? [:smm, a, b&.call] : super; end
L1I.setup
p [SI.new.j(1), KI.new.j(1)]

# the layers' bodies take every kind of parameter and pass them on, through an entry's arm, to a class with a prepend of
# its own and to a subclass
module L1J; def self.setup = (def j(a, d = 2, *r, k:, **kw, &b) = [:l1, a, d, r, k, kw, b&.call, super(a, d, *r, k: k, **kw, &b)]); end
module L2J; def self.setup = (def j(a, d = 2, *r, k:, **kw, &b) = [:l2, a, d, r, k, kw, b&.call]); end
module TMJ; def j(*a, **k, &b) = [:tm, super]; end
class KJ; end
class KJ; prepend L2J; end
class KJ; prepend L1J; end
class SJ < KJ; def self.arm = (def method_missing(m, *a, **k, &b) = m == :j ? [:smm, a] : super); end
class TJ < KJ; prepend TMJ; end
SJ.arm
L2J.setup
L1J.setup
p [SJ.new.j(1, 3, 4, k: 5, w: 6) { 7 }, KJ.new.j(1, k: 5), TJ.new.j(1, 3, k: 5) { 8 }]
