# find(ifnone) on a receiver whose type is known only at run time: the proc
# answers when nothing matches, and is not called when something does.
calls = 0
none = -> { calls += 1; :none }

def pick(x, ifnone)
  x.find(ifnone) { |v| v == 99 }
end

def pick_plain(x)
  x.find { |v| v == 99 }
end

def pick_last(x)
  x.rfind { |v| v.is_a?(Integer) && v > 1 }
end

p pick([1, 2, 3], none)
p pick([1, 99, 3], none)
p pick(1..3, none)
p pick({ a: 1 }, none)
p pick([], nil)
p pick([1, 2, 3], -> { nil })
p pick_plain([1, 99])
p pick_plain([1, 2])
p pick_plain(4..6)
p pick_last([1, 2, 3])
p pick_last([1])
p pick_last([5, "x", 7, "y"])
p calls

shown = [[1, 99], [2, 3]].map { |a| pick(a, none) }
p shown
p calls

# only nil means "no ifnone": any other fallback is called when nothing
# matches, so false raises NoMethodError for false.call (CRuby checks
# !NIL_P(if_none))
def pick_or_raise(x, ifnone)
  x.find(ifnone) { |v| v == 99 }
rescue NoMethodError
  "NoMethodError"
end
p pick_or_raise([1, 2, 3], false)
p pick_or_raise([1, 99], false)
p pick_or_raise(1..3, false)
p pick_or_raise(["a"], nil)
p pick_or_raise(["a"], false)
