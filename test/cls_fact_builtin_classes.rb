# The class facts say which builtin classes a value may be an instance of (analyze_cls.c, the bk part), so a
# boxed `x.send(:name)` beside a top-level def of the name tests only the builtin classes that can reach x,
# and calls the def at once where none can. Each source below is a builtin value the fact must keep, and has a
# method of its own: a send that skipped its class would call the top-level def where CRuby runs the
# builtin's method, and a source whose seed is dropped is not hidden by another that reaches the same method.
# The sources are literals, globals the runtime sets, constants, rescue bindings, parameters, a block's index
# and pair, class objects, what the share rows leave out (an Enumerator, a run of elements, a clamp's
# Range, `a[i, n] = [x]`, a splat's elements) and the program's subclasses of a builtin class.
def message = :top_message
def name = :top_name
def first(*) = :top_first
def succ = :top_succ
def call = :top_call
def fileno = :top_fileno
def peek = :top_peek
def default = :top_default
def next_float(*) = :top_nf
def bit_length(*) = :top_bl
def rotate(*) = :top_rotate
def keys(*) = :top_keys
def upcase(*) = :top_upcase
def five = 5
BINARY = 5
class A; end
class MyErr < StandardError; end
class Sub < StandardError; end
class MyA < Array; end
class MyH < Hash; end
class MyS < String; end
S = Struct.new(:a)
class Range; def extra = 1; end
class RuntimeError; def tag = 1; end
class ZeroDivisionError; def tag = 2; end
class Err < StandardError
  def initialize(n) = (@n = n; super("e"))
  def two = m_raise(@n)
end
def spread(a) = yield(*a)
def rest(*xs) = xs
def krest(**o) = o

def m0(o) = o.send(:fileno)
p [m0(A.new), m0($stdout)]
def m1(o) = o.send(:fileno)
p [m1(A.new), m1($stderr)]
def m2(o) = o.send(:fileno)
p [m2(A.new), m2(STDOUT)]
def m3(o) = o.send(:fileno)
p [m3(A.new), m3(STDERR)]
def m4(o) = o.send(:fileno)
p [m4(A.new), m4(STDIN)]
def m5(o) = o.send(:fileno)
$g = 5
p [m5(A.new), m5($g)]
def m6(o) = o.send(:first)
p [m6(A.new), m6(ARGV)]
def m7(o) = o.send(:fileno)
p [m7(A.new), m7(7)]
def m8(o) = o.send(:succ)
p [m8(A.new), m8(3)]
def m9(o) = o.send(:succ)
p [m9(A.new), m9("a")]
def m10(o) = o.send(:succ)
p [m10(A.new), m10(:b)]
def m11(o) = o.send(:succ)
p [m11(A.new), m11(2.5)]
def m12(o) = o.send(:succ)
p [m12(A.new), m12(nil)]
def m13(o) = o.send(:succ)
p [m13(A.new), m13((1..3))]
def m14(o) = o.send(:succ)
p [m14(A.new), m14(1r)]
def m15(o) = o.send(:succ)
p [m15(A.new), m15(true)]
def m16(o) = o.send(:message)
stop = begin; [].each.next; rescue StopIteration => e; e; end
p [m16(A.new), m16(stop)]
def m17(o) = o.send(:message)
err1 = begin; Integer("zz"); rescue => e; e; end
p [m17(A.new), m17(err1)]
def m18(o) = o.send(:message)
err2 = begin; 1 / 0; rescue ZeroDivisionError => e; e; end
p [m18(A.new), m18(err2)]
def m19(o) = o.send(:message)
p [m19(A.new), m19(MyErr.new("mine"))]
def m20(o) = o.send(:message)
p [m20(A.new), m20(RuntimeError.new("made"))]
def m21(o) = o.send(:message)
zde = begin; 1 / 0; rescue ZeroDivisionError => z; z; end
p [m21(A.new), m21(zde)]
def m22(o) = o.send(:name)
p [m22(A.new), m22(A)]
def m23(o) = o.send(:name)
p [m23(A.new), m23(String)]
def m24(o) = o.send(:name)
p [m24(A.new), m24(Comparable)]
def m25(o) = o.send(:name)
p [m25(A.new), m25(Encoding::BINARY)]
def m26(o) = o.send(:first)
p [m26(A.new), m26(rest(1, 2))]
def m27(o) = o.send(:first)
p [m27(A.new), m27(krest(a: 1))]
def m28(o) = o.send(:first)
p [m28(A.new), m28(S.new(9))]
def m29(o) = o.send(:succ)
idx = nil; [A.new].each_with_index { |_a, i| idx = i }
p [m29(A.new), m29(idx)]
def m30(o) = o.send(:first)
pr = nil; { k: 1 }.each { |pair| pr = pair }
p [m30(A.new), m30(pr)]
def m31(o) = o.send(:call)
p [m31(A.new), m31(proc { 5 })]
def m32(o) = o.send(:call)
p [m32(A.new), m32(-> { 6 })]
def m33(o) = o.send(:call)
p [m33(A.new), m33(method(:five))]
def m34(o) = o.send(:succ)
gs = nil; spread([8]) { |z| gs = z }
p [m34(A.new), m34(gs)]
def m35(o) = o.send(:succ)
h = {}; av = (h[:k] = 5)
p [m35(A.new), m35(av)]
def m36(o) = o.send(:peek)
p [m36(A.new), m36([1, 2].each)]
def m37(o) = o.send(:default)
p [m37(A.new), m37(Hash[[[1, 2]]])]
def m38(o) = o.send(:first)
p [m38(A.new), m38(Range.new(2, 3))]
def m39(o) = o.send(:succ)
p [m39(A.new), m39((/(?<w>b)/ =~ "abc"))]
def m40(o) = o.send(:next_float)
p [m40(A.new), m40(5.clamp(1.0..3.0))]
def m41(o) = o.send(:next_float)
p [m41(A.new), m41(5.clamp(..3.0))]
def m42(o) = o.send(:bit_length)
p [m42(A.new), m42(5.5.clamp(1..3))]
def m43(o) = o.send(:next_float)
ar = [1]; ar[0, 1] = [2.5]
p [m43(A.new), m43(ar[0])]
def m44(o) = o.send(:next_float)
ar = [1]; ar[0..0] = [3.5]
p [m44(A.new), m44(ar[0])]
def m45(o) = o.send(:rotate)
p [m45(A.new), m45(MyA.new.rotate!)]
def m46(o) = o.send(:rotate)
p [m46(A.new), m46(MyA.new.shuffle!)]
def m47(o) = o.send(:rotate)
p [m47(A.new), m47(MyA.new.tap { })]
def m48(o) = o.send(:rotate)
p [m48(A.new), m48(MyA.new.itself)]
def m49(o) = o.send(:rotate)
p [m49(A.new), m49(MyA.new.freeze)]
def m50(o) = o.send(:rotate)
p [m50(A.new), m50(MyA.new.dup)]
def m51(o) = o.send(:rotate)
p [m51(A.new), m51(MyA.new.clone)]
def m52(o) = o.send(:rotate)
p [m52(A.new), m52(MyA.new.each { })]
def m53(o) = o.send(:keys)
p [m53(A.new), m53(MyH.new.rehash)]
def m54(o) = o.send(:keys)
p [m54(A.new), m54(MyH.new.each { })]
def m55(o) = o.send(:upcase)
p [m55(A.new), m55(MyS.new("a").replace("b"))]
def m56(o) = o.send(:upcase)
p [m56(A.new), m56(MyS.new("a").itself)]
def m57(o) = o.send(:message)
p [m57(A.new), m57(Sub.new("x").exception)]
def m58(o) = o.send(:message)
p [m58(A.new), m58(Sub.new("x").itself)]

# raise K, arg builds K with the arguments
def m_raise(n) = n.send(:succ)
def mr(o) = o.send(:two)
p [(begin; raise Err, 21; rescue Err => e; mr(e); end)]

# public_send reaches only public methods: the private top-level def raises for A
def pm(o) = o.public_send(:fileno)
p [(pm(A.new) rescue :private), pm($stdout)]
