# A builtin on a yield whose block answers a different kind at each call
# site, where the program reopened one kind's class with its own method of
# that name. That site runs the reopen, so it is typed from the reopen's
# return, not from the builtin's: `class Integer; def abs = "int-abs"; end`
# boxed the String as an Integer and did not compile, and an Integer#itself
# reopen at one site handed its answer to the Float site too.

class Integer
  def abs = "int-abs"
  def itself = 5
  def -@ = [1]
end

class Float
  def magnitude = :mag
end

def absolute = yield.abs
p absolute { -3 }
p absolute { -2.5 }

def same = yield.itself
p same { 3 }
p same { 2.5 }

def negated = yield.-@
p negated { 5 }
p negated { 2.5 }

def size_of = yield.magnitude
p size_of { -3 }
p size_of { -2.5 }
