# The block operand of a split call, of each kind a local can hold, and the
# program's own methods named like the step's conversions.
def __blk_conv(x) = proc { |a| a * 100 }
def __kw_hash(x) = {z: 1}
class Tp
  def to_proc
    $h[:k] = 1
    proc { |*a| a }
  end
end
module M; T = Tp.new; end
def mm = M
def blk?(*a, **kw, &b) = b.nil?

e = {}
pr = proc { |a| a * 2 }
la = ->(a) { a + 1 }
s = :to_s
m = 5.method(:+)
hp = {1 => :a, 2 => :b}
o = nil
p [1, 2].map(**e, &pr)
p [1, 2].map(**e, &la)
p [1, 2].map(**e, &s)
p [1, 2].map(**e, &m)
p [1, 2].map(**e, &hp)
p blk?(**e, &o)
p blk?(**e, &pr)
p [1, 2].map(**e, &:to_s)

# a block operand with no #to_proc
class NoTp; end
n = NoTp.new
begin; [1].map(**e, &n); rescue TypeError => x; p x.message; end
i = 7
begin; [1].map(**e, &i); rescue TypeError => x; p x.message; end

# a constant path reads all the way up, or the call in it runs first
$h = {}; p [1].push(2, **$h, &::M::T)
$h = {}; p [1].push(2, **$h, &M::T)
$h = {}; p [1].push(2, **$h, &mm::T)
