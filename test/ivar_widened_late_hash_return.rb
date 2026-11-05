# A method's return type follows an ivar that widens after the return was
# first derived. `n` is the answer of a block call on a constant the program
# does not define (a library it does not load; the call is never reached),
# which inference never resolves, so the local is declared poly only in the
# late widening; the attr_accessor writer then widens @a to poly there, and
# `h`'s body becomes a Hash[String, untyped]. The return used to stay
# Hash[String, Integer], and the C did not build: `returning 'sp_StrPolyHash *'
# from a function with incompatible return type 'sp_StrIntHash *'`.
class T
  attr_accessor :a
  def initialize
    @a = 0
  end
  def h
    { "a" => @a }
  end
end
def run
  t = T.new
  n = DB.with { |c| c.changes }
  t.a = t.a + n
  t.h
end
p(ARGV.size > 5 ? run : 0)
