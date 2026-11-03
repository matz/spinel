# A #to_int that answers a non-Integer is named in CRuby's TypeError
# rb_to_int calls #to_int and judges the answer: "can't convert B to Integer
# (B#to_int gives String)", nil and Float included, and a Bignum answer is an
# Integer that then does not fit ("bignum too big to convert into 'long'").
# A boxed object whose #to_int was not statically an Integer had no row in
# the conversion bridge, so every such answer raised "no implicit conversion
# of B into Integer", and a Bignum one a TypeError.
def t(label)
  p [label, :ok, yield]
rescue TypeError, RangeError => e
  p [label, e.class, e.message]
end

class S; def to_int = "x"; end
class N; def to_int = nil; end
class F; def to_int = 1.5; end
class G; def to_int = 2**70; end
class C < S; end
class P
  def initialize(v) = @v = v
  def to_int = @v
end
module ToIntM; def to_int = :sym; end
class M; include ToIntM; end
class K; def to_int(&blk) = "k"; end
class I; def to_int = 1; end

a = [1, 2, 3]
t(:string) { a.fetch([S.new, 1][ARGV.size]) }
t(:nil) { a.fetch([N.new, 1][ARGV.size]) }
t(:float) { a.fetch([F.new, 1][ARGV.size]) }
t(:bignum) { a.fetch([G.new, 1][ARGV.size]) }
t(:subclass) { a.fetch([C.new, 1][ARGV.size]) }
t(:poly_int) { a.fetch([P.new(1), 1][ARGV.size]) }
t(:poly_string) { a.fetch([P.new("y"), 1][ARGV.size]) }
t(:poly_nil) { a.fetch([P.new(nil), 1][ARGV.size]) }
t(:module) { a.fetch([M.new, 1][ARGV.size]) }
t(:block_param) { a.fetch([K.new, 1][ARGV.size]) }
t(:integer) { a.fetch([I.new, 1][ARGV.size]) }
t(:pack) { [[S.new, 1][ARGV.size]].pack("C") }
t(:pack_int) { [[I.new, 1][ARGV.size]].pack("C").bytes }
