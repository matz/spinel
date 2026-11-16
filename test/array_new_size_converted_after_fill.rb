# Array.new(n, v) converts n (its #to_int, a boxed value's check, nil's
# TypeError) after it evaluates v: CRuby evaluates both arguments, then
# Array#initialize converts the size. The conversion ran ahead of v.
class Conv
  def initialize(n) = @n = n
  def to_int
    $log << :to_int
    @n
  end
end
$log = []
def fill(v) = ($log << :fill; v)

p Array.new(Conv.new(2), fill(1)), $log
$log = []
c = Conv.new(3)
p Array.new(c, fill("s")), $log
$log = []
# a size that may be either class, converted in the arm
def mk(n) = Array.new(n, fill(:x))
p mk(Conv.new(2)), mk(1), $log
$log = []
p Array.new(Conv.new(2), [fill(1.5)]), $log
$log = []
x = [1, nil][1]
begin
  Array.new(x, fill(0))
rescue TypeError => e
  $log << e.message
end
y = ["2", 3][0]
begin
  Array.new(y, fill(0))
rescue TypeError => e
  $log << e.message
end
p $log
# a nilable Integer (String#index's miss) is held with its nil
$log = []
begin
  Array.new("abc".index("z"), fill(0))
rescue TypeError => e
  $log << e.message
end
i = "abc".index("c")
p Array.new(i, fill(0)), $log
$log = []
begin
  Array.new(Conv.new(-1), fill(0))
rescue ArgumentError => e
  $log << e.message
end
p $log
# a fill with no effect of its own is read before #to_int changes it, and
# stays alive while #to_int drops its other holder and collects
class Swap
  def to_int
    $fill = :new
    $s = "replaced"
    GC.start
    2
  end
end
$fill = :old
p Array.new(Swap.new, $fill)
$s = "a" * 3
p Array.new(Swap.new, $s)
# a plain Integer size and a fill without side effects keep their order
p Array.new(2, fill(7)), Array.new(Conv.new(2), 0)
