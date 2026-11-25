# A call whose receiver is a call expression reaches method_missing where
# the value is of a class that lacks the method, as a local receiver does:
# the receiver runs once, ahead of the arguments.
class Ghost
  def method_missing(name, *args, &blk)
    name.to_s.start_with?("boo") ? "mm:#{name}:#{args.inspect}" : super
  end
  def respond_to_missing?(n, p = false) = n.to_s.start_with?("boo") || super
end
class Real
  def boo(*a) = "real:#{a.inspect}"
  def boo2 = "real2"
end
$log = []
def pick(i) = ($log << "pick#{i}"; i.even? ? Ghost.new : Real.new)
def arg(v) = ($log << "arg#{v}"; v)
p pick(0).boo(arg(1), arg(2))
p pick(1).boo(arg(3))
p $log
p [0, 1, 2].map { |i| pick(i).boo }
x = nil
p x&.boo
def maybe(i) = i == 0 ? nil : pick(i)
p maybe(0)&.boo
p maybe(2)&.boo
p pick(2).boo.size
i = 0
r = []
while (s = pick(i).boo2) && i < 2
  r << s
  i += 1
end
p r
h = { a: pick(0) }
p h[:a].boo
p (pick(4).boo rescue "x")
begin
  pick(0).nope
rescue NoMethodError => e
  p e.message[0, 22]
end
