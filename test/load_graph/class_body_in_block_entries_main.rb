# A class body in a block (a lambda, a Proc, an exit handler) is its class's
# body: each kind of entry it holds applies once the block has run.
module Mx
  def mx = "mx"
end
class VisNames
  def baz0 = "baz"
end
class VisPublic
  def baz1 = "baz"
  private def pv1
    "pv1"
  end
end
class VisProtected
  def baz2 = "baz"
end
class SectionDef
  def baz3 = "baz"
end
class PrefixDef
  def baz4 = "baz"
end
class PublicAgain
  def baz5 = "baz"
end
class AliasMethod
  def baz6 = "baz"
end
class AliasKw
  def baz7 = "baz"
end
class UndefMethod
  def baz8 = "baz"
end
class UndefKw
  def baz9 = "baz"
end
class AttrReader
  def baz10 = "baz"
end
class AttrAccessor
  def baz11 = "baz"
end
class DefineMethod
  def baz12 = "baz"
end
class Includer
  def baz13 = "baz"
end
class DefInst
  def baz14 = "baz"
end
class DefSelf
  def baz15 = "baz"
end
class SclassDef
  def baz16 = "baz"
end
module ModSection
end
module ModNames
  def mf0 = "mf0"
end
def show(tag)
  yield
rescue NameError => e
  p [tag, e.class.name]
end
b0 = -> {
  class VisNames
    private :baz0
  end
  nil
}
b1 = Proc.new do
  class VisPublic
    public :pv1
  end
  nil
end
b2 = -> {
  class VisProtected
    protected :baz2
  end
  nil
}
b3 = Proc.new do
  class SectionDef
    private
    def nw3 = "nw3"
  end
  nil
end
b4 = -> {
  class PrefixDef
    private def nw4
      "nw4"
    end
  end
  nil
}
b5 = Proc.new do
  class PublicAgain
    private
    def a15 = 1
    public
    def nw5 = "nw5"
  end
  nil
end
b6 = -> {
  class AliasMethod
    alias_method :bz6, :baz6
  end
  nil
}
b7 = Proc.new do
  class AliasKw
    alias bz7 baz7
  end
  nil
end
b8 = -> {
  class UndefMethod
    undef_method :baz8
  end
  nil
}
b9 = Proc.new do
  class UndefKw
    undef baz9
  end
  nil
end
b10 = -> {
  class AttrReader
    attr_reader :ar10
  end
  nil
}
b11 = Proc.new do
  class AttrAccessor
    attr_accessor :ac11
  end
  nil
end
b12 = -> {
  class DefineMethod
    define_method(:dm12) { "dm12" }
  end
  nil
}
b13 = Proc.new do
  class Includer
    include Mx
  end
  nil
end
b14 = -> {
  class DefInst
    def nw14 = "nw14"
  end
  nil
}
b15 = Proc.new do
  class DefSelf
    def self.cs15 = "cs15"
  end
  nil
end
b16 = -> {
  class SclassDef
    class << self
      def cs216 = "cs216"
    end
  end
  nil
}
mods = -> {
  module ModSection
    module_function
    def mf = "mf"
  end
  module ModNames
    module_function :mf0
  end
  nil
}
# before the blocks run: no class method of the two defs yet
show("pre DefSelf.cs15") { p ["pre DefSelf.cs15", DefSelf.cs15] }
show("pre DefSelf.respond_to?(:cs15)") { p ["pre DefSelf.respond_to?(:cs15)", DefSelf.respond_to?(:cs15)] }
show("pre SclassDef.cs216") { p ["pre SclassDef.cs216", SclassDef.cs216] }
show("pre SclassDef.respond_to?(:cs216)") { p ["pre SclassDef.respond_to?(:cs216)", SclassDef.respond_to?(:cs216)] }
b0.call
b1.call
b2.call
b3.call
b4.call
b5.call
b6.call
b7.call
b8.call
b9.call
b10.call
b11.call
b12.call
b13.call
b14.call
b15.call
b16.call
mods.call
show("VisNames.new.baz0") { p ["VisNames.new.baz0", VisNames.new.baz0] }
show("VisNames.new.respond_to?(:baz0)") { p ["VisNames.new.respond_to?(:baz0)", VisNames.new.respond_to?(:baz0)] }
show("VisNames.method_defined?(:baz0)") { p ["VisNames.method_defined?(:baz0)", VisNames.method_defined?(:baz0)] }
show("VisNames.private_method_defined?(:baz0)") { p ["VisNames.private_method_defined?(:baz0)", VisNames.private_method_defined?(:baz0)] }
show("VisPublic.new.pv1") { p ["VisPublic.new.pv1", VisPublic.new.pv1] }
show("VisPublic.new.respond_to?(:pv1)") { p ["VisPublic.new.respond_to?(:pv1)", VisPublic.new.respond_to?(:pv1)] }
show("VisPublic.method_defined?(:pv1)") { p ["VisPublic.method_defined?(:pv1)", VisPublic.method_defined?(:pv1)] }
show("VisPublic.private_method_defined?(:pv1)") { p ["VisPublic.private_method_defined?(:pv1)", VisPublic.private_method_defined?(:pv1)] }
show("VisProtected.new.baz2") { p ["VisProtected.new.baz2", VisProtected.new.baz2] }
show("VisProtected.new.respond_to?(:baz2)") { p ["VisProtected.new.respond_to?(:baz2)", VisProtected.new.respond_to?(:baz2)] }
show("VisProtected.method_defined?(:baz2)") { p ["VisProtected.method_defined?(:baz2)", VisProtected.method_defined?(:baz2)] }
show("VisProtected.private_method_defined?(:baz2)") { p ["VisProtected.private_method_defined?(:baz2)", VisProtected.private_method_defined?(:baz2)] }
show("VisProtected.protected_method_defined?(:baz2)") { p ["VisProtected.protected_method_defined?(:baz2)", VisProtected.protected_method_defined?(:baz2)] }
show("SectionDef.new.nw3") { p ["SectionDef.new.nw3", SectionDef.new.nw3] }
show("SectionDef.new.respond_to?(:nw3)") { p ["SectionDef.new.respond_to?(:nw3)", SectionDef.new.respond_to?(:nw3)] }
show("SectionDef.method_defined?(:nw3)") { p ["SectionDef.method_defined?(:nw3)", SectionDef.method_defined?(:nw3)] }
show("SectionDef.private_method_defined?(:nw3)") { p ["SectionDef.private_method_defined?(:nw3)", SectionDef.private_method_defined?(:nw3)] }
show("PrefixDef.new.nw4") { p ["PrefixDef.new.nw4", PrefixDef.new.nw4] }
show("PrefixDef.new.respond_to?(:nw4)") { p ["PrefixDef.new.respond_to?(:nw4)", PrefixDef.new.respond_to?(:nw4)] }
show("PrefixDef.method_defined?(:nw4)") { p ["PrefixDef.method_defined?(:nw4)", PrefixDef.method_defined?(:nw4)] }
show("PrefixDef.private_method_defined?(:nw4)") { p ["PrefixDef.private_method_defined?(:nw4)", PrefixDef.private_method_defined?(:nw4)] }
show("PublicAgain.new.nw5") { p ["PublicAgain.new.nw5", PublicAgain.new.nw5] }
show("PublicAgain.new.respond_to?(:nw5)") { p ["PublicAgain.new.respond_to?(:nw5)", PublicAgain.new.respond_to?(:nw5)] }
show("PublicAgain.method_defined?(:nw5)") { p ["PublicAgain.method_defined?(:nw5)", PublicAgain.method_defined?(:nw5)] }
show("PublicAgain.private_method_defined?(:nw5)") { p ["PublicAgain.private_method_defined?(:nw5)", PublicAgain.private_method_defined?(:nw5)] }
show("AliasMethod.new.bz6") { p ["AliasMethod.new.bz6", AliasMethod.new.bz6] }
show("AliasMethod.new.respond_to?(:bz6)") { p ["AliasMethod.new.respond_to?(:bz6)", AliasMethod.new.respond_to?(:bz6)] }
show("AliasMethod.method_defined?(:bz6)") { p ["AliasMethod.method_defined?(:bz6)", AliasMethod.method_defined?(:bz6)] }
show("AliasMethod.private_method_defined?(:bz6)") { p ["AliasMethod.private_method_defined?(:bz6)", AliasMethod.private_method_defined?(:bz6)] }
show("AliasKw.new.bz7") { p ["AliasKw.new.bz7", AliasKw.new.bz7] }
show("AliasKw.new.respond_to?(:bz7)") { p ["AliasKw.new.respond_to?(:bz7)", AliasKw.new.respond_to?(:bz7)] }
show("AliasKw.method_defined?(:bz7)") { p ["AliasKw.method_defined?(:bz7)", AliasKw.method_defined?(:bz7)] }
show("AliasKw.private_method_defined?(:bz7)") { p ["AliasKw.private_method_defined?(:bz7)", AliasKw.private_method_defined?(:bz7)] }
show("UndefMethod.new.baz8") { p ["UndefMethod.new.baz8", UndefMethod.new.baz8] }
show("UndefMethod.new.respond_to?(:baz8)") { p ["UndefMethod.new.respond_to?(:baz8)", UndefMethod.new.respond_to?(:baz8)] }
show("UndefMethod.method_defined?(:baz8)") { p ["UndefMethod.method_defined?(:baz8)", UndefMethod.method_defined?(:baz8)] }
show("UndefMethod.private_method_defined?(:baz8)") { p ["UndefMethod.private_method_defined?(:baz8)", UndefMethod.private_method_defined?(:baz8)] }
show("UndefKw.new.baz9") { p ["UndefKw.new.baz9", UndefKw.new.baz9] }
show("UndefKw.new.respond_to?(:baz9)") { p ["UndefKw.new.respond_to?(:baz9)", UndefKw.new.respond_to?(:baz9)] }
show("UndefKw.method_defined?(:baz9)") { p ["UndefKw.method_defined?(:baz9)", UndefKw.method_defined?(:baz9)] }
show("UndefKw.private_method_defined?(:baz9)") { p ["UndefKw.private_method_defined?(:baz9)", UndefKw.private_method_defined?(:baz9)] }
show("AttrReader.new.ar10") { p ["AttrReader.new.ar10", AttrReader.new.ar10] }
show("AttrReader.new.respond_to?(:ar10)") { p ["AttrReader.new.respond_to?(:ar10)", AttrReader.new.respond_to?(:ar10)] }
show("AttrReader.method_defined?(:ar10)") { p ["AttrReader.method_defined?(:ar10)", AttrReader.method_defined?(:ar10)] }
show("AttrReader.private_method_defined?(:ar10)") { p ["AttrReader.private_method_defined?(:ar10)", AttrReader.private_method_defined?(:ar10)] }
show("AttrAccessor.new.ac11") { p ["AttrAccessor.new.ac11", AttrAccessor.new.ac11] }
show("AttrAccessor.new.respond_to?(:ac11)") { p ["AttrAccessor.new.respond_to?(:ac11)", AttrAccessor.new.respond_to?(:ac11)] }
show("AttrAccessor.method_defined?(:ac11)") { p ["AttrAccessor.method_defined?(:ac11)", AttrAccessor.method_defined?(:ac11)] }
show("AttrAccessor.private_method_defined?(:ac11)") { p ["AttrAccessor.private_method_defined?(:ac11)", AttrAccessor.private_method_defined?(:ac11)] }
show("AttrAccessor.method_defined?(:ac11=)") { p ["AttrAccessor.method_defined?(:ac11=)", AttrAccessor.method_defined?(:ac11=)] }
show("DefineMethod.new.dm12") { p ["DefineMethod.new.dm12", DefineMethod.new.dm12] }
show("DefineMethod.new.respond_to?(:dm12)") { p ["DefineMethod.new.respond_to?(:dm12)", DefineMethod.new.respond_to?(:dm12)] }
show("DefineMethod.method_defined?(:dm12)") { p ["DefineMethod.method_defined?(:dm12)", DefineMethod.method_defined?(:dm12)] }
show("DefineMethod.private_method_defined?(:dm12)") { p ["DefineMethod.private_method_defined?(:dm12)", DefineMethod.private_method_defined?(:dm12)] }
show("Includer.new.mx") { p ["Includer.new.mx", Includer.new.mx] }
show("Includer.include?(Mx)") { p ["Includer.include?(Mx)", Includer.include?(Mx)] }
show("Includer.new.is_a?(Mx)") { p ["Includer.new.is_a?(Mx)", Includer.new.is_a?(Mx)] }
show("DefInst.new.nw14") { p ["DefInst.new.nw14", DefInst.new.nw14] }
show("DefInst.new.respond_to?(:nw14)") { p ["DefInst.new.respond_to?(:nw14)", DefInst.new.respond_to?(:nw14)] }
show("DefInst.method_defined?(:nw14)") { p ["DefInst.method_defined?(:nw14)", DefInst.method_defined?(:nw14)] }
show("DefInst.private_method_defined?(:nw14)") { p ["DefInst.private_method_defined?(:nw14)", DefInst.private_method_defined?(:nw14)] }
show("DefSelf.cs15") { p ["DefSelf.cs15", DefSelf.cs15] }
show("DefSelf.respond_to?(:cs15)") { p ["DefSelf.respond_to?(:cs15)", DefSelf.respond_to?(:cs15)] }
show("SclassDef.cs216") { p ["SclassDef.cs216", SclassDef.cs216] }
show("SclassDef.respond_to?(:cs216)") { p ["SclassDef.respond_to?(:cs216)", SclassDef.respond_to?(:cs216)] }
show("ModSection.mf") { p ["ModSection.mf", ModSection.mf] }
show("ModSection.respond_to?(:mf)") { p ["ModSection.respond_to?(:mf)", ModSection.respond_to?(:mf)] }
show("ModSection.private_method_defined?(:mf)") { p ["ModSection.private_method_defined?(:mf)", ModSection.private_method_defined?(:mf)] }
show("ModNames.mf0") { p ["ModNames.mf0", ModNames.mf0] }
show("ModNames.respond_to?(:mf0)") { p ["ModNames.respond_to?(:mf0)", ModNames.respond_to?(:mf0)] }
show("ModNames.private_method_defined?(:mf0)") { p ["ModNames.private_method_defined?(:mf0)", ModNames.private_method_defined?(:mf0)] }
# A def in a class body in a Proc that a visibility word there names
# later: once the Proc has run, the method answers as private or
# protected.
class LaterVis
end
class LaterProt
end
lv = LaterVis.new
lp = LaterProt.new
Proc.new do
  class LaterVis
    def lm = :k
    private :lm
  end
  class LaterProt
    def lm = :k
    protected :lm
  end
  nil
end.call
p [lv.respond_to?(:lm) ? lv.lm : :none, LaterVis.private_method_defined?(:lm), defined?(lv.lm)]
p [lp.respond_to?(:lm), (lp.lm rescue $!.class), LaterProt.protected_method_defined?(:lm)]
# Entries of one name in other places than the def's class side: a
# define_singleton_method next to the def, and a private word in another
# class's body in a lambda. Each answers as CRuby does once the blocks
# have run.
class DsmBoth
end
class CrossA
end
class CrossB
  def cm = :b0
end
db = DsmBoth.new
cb = CrossB.new
Proc.new do
  class DsmBoth
    def dm = :k
    define_singleton_method(:dm) { :s }
  end
  class CrossA
    def cm = :a
  end
  nil
end.call
-> { class CrossB; private :cm; end; nil }.call
p [db.respond_to?(:dm), db.dm, DsmBoth.respond_to?(:dm), DsmBoth.dm]
p [cb.respond_to?(:cm), (cb.cm rescue $!.class), CrossA.new.cm]
# A top-level def, a private method of Object, and a `public def` there,
# next to a def of the same name in a class body in a Proc: once the Proc
# has run, the class's method answers, and Object's still answers for
# respond_to? with private methods.
def md1 = :main
public def md8 = :main
class MainDef
end
mdo = MainDef.new
mdx = [mdo, 1]
Proc.new do
  class MainDef
    def md1 = :k
    def md8 = :k
  end
  nil
end.call
p [mdo.respond_to?(:md1), mdx.map { |x| x.respond_to?(:md1) ? x.md1 : :none }, (mdo.md1 rescue $!.class), defined?(mdo.md1), mdx.map { |x| x.respond_to?(:md1, true) }]
p [mdo.respond_to?(:md8), (mdo.md8 rescue $!.class)]
# A `module Kernel` def, and a def of the same name in a class body in a
# Proc, asked only by a respond_to? in a method, and asked and called in
# a lambda and in a method: Kernel's method answers before the Proc runs,
# the class's after.
module Kernel
  def kr1 = :ker
end
class KerResp
end
def kr_chk(k) = k.respond_to?(:kr1)
kro = KerResp.new
kpr = proc do
  class KerResp
    def kr1 = :k
  end
  nil
end
p kr_chk(kro)
kpr.call
p kr_chk(kro)
module Kernel
  def kr2 = :ker
end
class KerCall
end
krc = KerCall.new
kpr2 = proc do
  class KerCall
    def kr2 = :k
  end
  nil
end
kr_out = -> { [krc.respond_to?(:kr2), (krc.kr2 rescue $!.class)] }
def kr_call(k) = [k.respond_to?(:kr2), (k.kr2 rescue $!.class)]
p kr_out.call, kr_call(krc)
kpr2.call
p kr_out.call, kr_call(krc)
# A main `send(:define_method)` of a name a class body in a Proc defines
# too: respond_to? answers Object's method before the Proc runs.
send(:define_method, :sdm) { :main } rescue p $!.class
class SendDm
end
sdo = SendDm.new
sdp = proc do
  class SendDm
    def sdm = :k
  end
  nil
end
p sdo.respond_to?(:sdm)
sdp.call
p sdo.respond_to?(:sdm), sdo.sdm
# A def in a class body in a Proc, of a name an earlier class body
# undefined: a boxed receiver's respond_to? answers as a typed one does.
class UndefThen
  def ut = :t
  undef_method :ut
end
uto = UndefThen.new
utx = [uto, 1]
utp = proc do
  class UndefThen
    def ut = :k
  end
  nil
end
p [uto.respond_to?(:ut), utx.map { |x| x.respond_to?(:ut) ? x.ut : :none }, utx.map { |x| x.respond_to?(:ut, true) }]
utp.call
p [uto.respond_to?(:ut), utx.map { |x| x.respond_to?(:ut) ? x.ut : :none }, utx.map { |x| x.respond_to?(:ut, true) }]
# The same def, undefined after its Proc ran and made again, asked from
# a lambda: before the Proc runs no method answers.
class UndefAfter
end
uao = UndefAfter.new
uax = [uao, 1]
uap = proc do
  class UndefAfter
    def ua = :k
  end
  nil
end
uachk = -> { p [uao.respond_to?(:ua), (uao.ua rescue $!.class), uax.map { |x| x.respond_to?(:ua) ? x.ua : :none }] }
uachk.call
uap.call
uachk.call
class UndefAfter
  undef_method :ua
end
uachk.call
uap.call
uachk.call
class AtExitH
  def baz = "baz"
end
at_exit do
  show("exit private") { p AtExitH.new.baz }
  show("exit alias") { p AtExitH.new.bz }
end
at_exit do
  class AtExitH
    alias_method :bz, :baz
    private :baz
  end
  nil
end
# A def in a class body in one exit handler, called from one that runs
# after it (exit handlers run last registered first) and from a method
# such a handler calls: the call answers the def's method.
class AtExitD
  def baz = "baz"
end
class AtExitS
  def self.n = :old
end
def at_exit_chk = [AtExitD.new.baz, (AtExitS.m rescue $!.class), AtExitS.respond_to?(:m), AtExitS.n]
at_exit do
  show("exit def") { p at_exit_chk }
  show("exit def direct") { p [AtExitD.new.baz, AtExitS.m] }
end
at_exit do
  class AtExitD
    def baz = "new"
  end
  class AtExitS
    def self.m = :m
    class << self
      def n = :new
    end
  end
  nil
end
