# The boxed argument of method_capture_boxed_arg.rb, into a method that hands
# it down nine more, each defined before its caller. The widened parameter
# crosses one call a round, behind the Integer the direct call left a round
# earlier: stopped before the last method, `leaf` kept an Integer parameter
# and the call into it turned the Float into 1 and the String into 0.
def leaf(value) = value
def step1(value) = leaf(value)
def step2(value) = step1(value)
def step3(value) = step2(value)
def step4(value) = step3(value)
def step5(value) = step4(value)
def step6(value) = step5(value)
def step7(value) = step6(value)
def step8(value) = step7(value)
def step9(value) = step8(value)
def adapter(box)
  callback = box[:load]
  proc { |value| callback.call(value) }
end
p step9(0)
f = adapter({load: method(:step9)})
p f.call(1.5)
p f.call("s")
p step9(2)
