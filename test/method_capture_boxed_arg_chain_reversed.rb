# method_capture_boxed_arg_chain.rb with each method defined after its
# caller: the same answers in either order of definition.
def step9(value) = step8(value)
def step8(value) = step7(value)
def step7(value) = step6(value)
def step6(value) = step5(value)
def step5(value) = step4(value)
def step4(value) = step3(value)
def step3(value) = step2(value)
def step2(value) = step1(value)
def step1(value) = leaf(value)
def leaf(value) = value
def adapter(box)
  callback = box[:load]
  proc { |value| callback.call(value) }
end
p step9(0)
f = adapter({load: method(:step9)})
p f.call(1.5)
p f.call("s")
p step9(2)
