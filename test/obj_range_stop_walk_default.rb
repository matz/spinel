# A find in a parameter default, and any?/all?/none?/one? with a block,
# over a Range of the program's objects call succ only up to the member
# that stops the walk, as CRuby does. An endless Range works.
class N
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ
    $log << @n
    N.new(@n + 1)
  end
end

def found(r, k = r.find { |v| v.n > 1 }) = k&.n
def kw(r, k: r.detect { |v| v.n * 2 > 5 }) = k.n

$log = []
p found(N.new(0)..N.new(5)), $log
$log = []
p found(N.new(0)..), $log
$log = []
p found(N.new(0)..N.new(5), N.new(9)), $log
$log = []
p kw(N.new(0)..N.new(9)), $log
$log = []
p found(N.new(3)..N.new(4)), $log

r = N.new(0)..N.new(5)
$log = []
p r.any? { |v| v.n > 1 }, $log
$log = []
p r.all? { |v| v.n < 2 }, $log
$log = []
p r.none? { |v| v.n == 1 }, $log
$log = []
p r.one? { |v| v.n > 2 }, $log
$log = []
p((N.new(0)..).any? { |v| v.n > 2 }, $log)
$log = []
p((N.new(0)...N.new(3)).any? { |v| v.n > 7 }, $log)
