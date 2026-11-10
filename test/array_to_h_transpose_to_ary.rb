# Array#to_h and Array#transpose convert a program object through its
# to_ary, public or private, as CRuby does.
class P
  def initialize(a) = @a = a
  def to_ary = @a
end
class Q
  def initialize(a) = @a = a
  private def to_ary = @a
end
class N
  def to_ary = nil
end
class W
  def to_ary = 5
end
class Z; end

p [P.new([1, 2])].to_h
p [Q.new([:a, 2]), [:b, 3]].to_h
p [1, 2].to_h { |x| Q.new([x, x * 10]) }
p [Q.new(%w[k v])].to_h { |x| x }
p [[1, 2], P.new([3, 4])].transpose
p [Q.new([1, 2]), Q.new([3, 4])].transpose
p [Q.new(["a", "b"]), ["c", "d"]].transpose

def show
  yield
rescue => e
  p [e.class, e.message]
end
show { [N.new].to_h }
show { [W.new].to_h }
show { [Z.new].to_h }
show { [Q.new([1])].to_h }
show { [Q.new([1, 2]), 5].to_h }
show { [[1, 2], Z.new].transpose }
show { [[1, 2], W.new].transpose }
show { [[1, 2], Q.new([3])].transpose }

# each to_ary runs just before its element's check, as in CRuby
class L
  def initialize(n, a) = (@n = n; @a = a)
  def to_ary
    puts "to_ary #{@n}"
    @a
  end
end
show { [L.new(1, [1, 2]), 5, L.new(2, [3, 4])].to_h }
show { [L.new(1, [1]), L.new(2, [3, 4])].to_h }
show { [L.new(1, [1, 2]), [3], L.new(2, [5, 6])].transpose }
show { [N.new].transpose }

# a to_ary that shortens the receiver: the live length is read
class S
  def initialize(n) = @n = n
  def to_ary
    $arr.pop if $arr.size > 3
    [@n, @n]
  end
end
$arr = [[0, 0]] + (1..5).map { |i| S.new(i) }
p $arr.to_h

# a row is read when transpose reaches it: a later to_ary that changes it
# changes nothing
$r = [1, 2]
class M; def to_ary = ($r[0] = 99; [3, 4]); end
class G; def to_ary = ($r << 5; [3, 4]); end
p [$r, M.new].transpose
$r = [1, 2]
p [$r, G.new].transpose
