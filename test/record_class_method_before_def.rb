# A class method of a Struct or Data class, called from a method or a
# lambda before the program's def of it, raises NoMethodError, then runs
# the def once it exists. The chain past the record class is the builtin
# Struct or Data, which has no such method.

S = Struct.new(:a)
def call_s = S.late
p((call_s rescue :nme))
def S.late = 1
p call_s

D = Data.define(:x)
lam = -> { D.d_late }
p((lam.call rescue :nme))
def D.d_late = 2
p lam.call

# a header whose superclass is the class the builder makes
class T < Struct.new(:a); end
def call_t = T.t_late
p((call_t rescue :nme))
def T.t_late = 3
p call_t

# a subclass of a record class
U = Struct.new(:a)
class V < U; end
def call_v = V.u_late
p((call_v rescue :nme))
def U.u_late = 4
p call_v

# the builtin class methods answer before any def
p S.members, D.members

# respond_to? of a class method a method body defines answers as that body
# has run: from a method, beside a class-side attr, beside a class method
# in the block, and receiverless in the block's class method
RA = Struct.new(:a)
def set_ra = (def RA.k = :ra)
def go_ra = (RA.respond_to?(:k) ? RA.k : :none)
p go_ra
set_ra
p go_ra

RB = Struct.new(:a) do
  class << self
    attr_accessor :cfg
  end
end
RB.cfg = 3
def set_rb = (def RB.k = :rb)
p RB.cfg, RB.respond_to?(:cfg), RB.respond_to?(:k)

RC = Struct.new(:a) do
  def self.j = :j
end
def set_rc = (def RC.k = :rc)
def go_rc = (RC.respond_to?(:k) ? RC.k : :none)
p go_rc
set_rc
p go_rc

RD = Struct.new(:a) do
  def self.j = (respond_to?(:k) ? :yes : :no)
end
def set_rd = (def RD.k = :rd)
p RD.j
set_rd
p RD.j
