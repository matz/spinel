# A tuple taken apart in a conditional's arm whose value is thrown away, or
# as the last statement of the block of a builtin walk that ignores its
# block's value, keeps its table. Where the conditional's, the block's or the
# method's value is read, or a break hands the tuple on, the table stays
# boxed and answers as CRuby does. Each case has a method of its own: one
# call that hands the tuple on keeps every call of its method boxed.
# spinel: gc-stress

def arm_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
if ARGV.empty?
  ar_a, ar_b = arm_pair
end
ar_a[0][0] += 1
p ar_a, ar_b

def walk_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 3]
end
wk_a = nil
1.times { |i| wk_a, wk_b = walk_pair }
wk_a[1][1] += 4
p wk_a

# the if's value is read
def if_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
if_x = if ARGV.empty?
  if_a, if_b = if_pair
end
if_x[0][0][0] = "z"
p if_a, if_x[1]

# the case's value is read
def case_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
cs_x = case ARGV.size
       when 0 then cs_a, cs_b = case_pair
       end
cs_x[0][1][1] = "w"
p cs_a

# the && reads it
def and_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
an_x = ARGV.empty? && (an_a, an_b = and_pair)
an_x[0][1][0] = "v"
p an_a

# an if that is a method's last statement
def last_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
def last_if
  if ARGV.empty?
    la_a, la_b = last_pair
  end
end
la_x = last_if
la_x[0][0][1] = "u"
p la_x

# a begin's value is read
def begin_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
bg_x = begin
  bg_a, bg_b = begin_pair
rescue
  nil
end
bg_x[0][0][0] = "r"
p bg_a

# a map block answers it
def map_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
mp_x = [1].map { |i| if i > 0 then mp_a, mp_b = map_pair end }
mp_x[0][0][0][0] = "m"
p mp_x

# the block of a program's own method that reads what its block answers
def yield_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
def answer_of
  yield
end
yd_x = answer_of { if true then yd_a, yd_b = yield_pair end }
yd_x[0][0][0] = "y"
p yd_x

# a break hands it on
def break_pair
  t = Array.new(2) { Array.new(2, 1) }
  [t, 2]
end
bk_x = 1.times { |i| break (bk_a, bk_b = break_pair) }
bk_x[0][0][0] = "b"
p bk_x
