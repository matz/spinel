# Every shape tools/implied_calls.yml has a row for, once, for
# `spinel --dump-implied-calls` (make implied-calls-test). The program is only
# parsed: it is not meant to run, and a name that nothing defines stays one.

class Base; end
class K < Base
  X = 1
  X ||= 2
  X &&= 3
  X += 4
  def m; end
  def self.s; end
  def o.single; end
  alias n m
  undef m
end
module M; end
A::B = 1
A::B ||= 2
A::B += 3
C, D = 1, 2
A::E, g = 1, 2
Z

o = Object.new
pinned = 5
s = "a#{o}b#{1}c#{(2)}d#{'e'}#{o; 3}"
t = "#@v #$w"
u = :"x#{o}"
r = /x#{o}y/
cmd = `ls #{o}`
cmd2 = `ls`

args = [*o, *1, *[o]]
call(*o, **o)
call(&o)
call(**{a: 1}, &:sym)
call(&nil)
h = { **o, **nil }
def fwd(*, **, &) = call(*, **, &)
a, b = o
a, b = 1
a, b = 1, 2
a, = o
a, (b, c) = o
*a, b = o
x, *y = o

blk = proc { |a, b| }
blk = proc { |a| }
blk = proc { |*a| }
blk = proc { |a, | }
blk = proc { |a, *b| }
blk = proc { |(a, b)| }
blk = proc { _1 + _2 }
blk = proc { _1 }

for i in o do end
for i, j in o do end

case 1
when o, K, *list then 1
end
begin
rescue K, *list => e
rescue M
end

q = (1..2)
q = (1..o)
q = (o...o)
q = (o..)
q = (..o)

case o
in [a, b] then 1
in [Integer => a, *] then 2
in {k: 5, j: ^pinned} then 3
in K[a] then 4
in K(a:) then 5
in [*, 1, *] then 6
in 1 | 2 then 7
in K[*, 1, *] then 8
in x then 9
end
case o
in 1 then 1
else 2
end
o => [first]
o in {z:}
o => ^(1 + 1)

p defined?(o.zz)
p defined?(zz)
p defined?(@x)
p defined?(o.zz.yy.xx)
p defined?(zz.foo)
p defined?(o.zz(1))

h = { o => 1, K => 2, "s" => 3, sym: 4, 1 => 5, [o] => 6 }
call(o => 1, sym: 2)

o.attr += 1
o.attr ||= 2
o.attr &&= 3
o.attr, y = 1, 2
o[1] += 1
o[1] ||= 2
o[1] &&= 3
o[1], y = 1, 2
n = 0
n += 1
@n += 1
@@n += 1
$n += 1
$stdout = o
$stderr ||= o
$> &&= o
$stdout += 1
$stdout, z = o, 1
$other = o
[*((((((((nil))))))))]
"#{((((((((1))))))))}"
[*((((((((((nil))))))))))]
"#{((((((((((1))))))))))}"
[*((((((((o))))))))]
"#{((((((((o))))))))}"
